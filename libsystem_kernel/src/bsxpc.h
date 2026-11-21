/*
 * bsxpc.h: BSXPC coder layer for ioswine.
 *
 * The bsxpc_envelope_keys struct holds the five real key strings
 * measured on device 2026-11-12; bsxpc_message_parse /
 * bsxpc_reply_build are the envelope API. The reply shape (result
 * fields at the top level of the reply dict, not nested under
 * bsxpc_context) is [guess]: the exact reply dict is unobserved;
 * bsxpc_context is documented as the handshake dict, not a general
 * result container.
 *
 * Builder/reader helpers on top of the Part A xpcwire library for the
 * BaseBoard object encodings that FrontBoard/RunningBoard traffic
 * carries inside XPC dictionaries: BSSettings (typed key/value
 * store), BSAction (scene-action carrier), RBSProcessHandle
 * (process identity plus audit token), audit tokens, and the
 * BSXPCServiceConnection message envelope. Also the verbatim
 * message trace logger.
 *
 * Wire keys are PARAMETERS, never hardcoded here. Every function
 * that writes or reads a named field takes the key string from the
 * caller; the fakeboard names table (fakeboard_names.h)
 * supplies them, so a wrong string can be corrected after the first
 * device trace without touching this file. The one exception is the
 * geometry field names ("width", "height", "x", "y", "top", "left",
 * "bottom", "right") inside size/rect/point/insets values: those are
 * part of the encoding definition in this file, not per-setting
 * keys, and are documented as [guess] below.
 *
 * Style: freestanding C, same as xpcwire.c/launchd.c. No libc. All
 * helpers are bs_ or bsxpc_ prefixed. No unaligned reads anywhere;
 * every read is bounds checked by the xpcwire reader underneath.
 * Every function returns XW_OK (0) or a negative XW_ERR_* from
 * xpcwire.h, unless noted.
 *
 * Sources and [guess] list (read before trusting any wire string):
 *
 * - BSSettings conforms to BSXPCCoding / BSXPCSecureCoding and has
 *   +settingsFromXPCDictionary: and -initWithXPCDictionary:; its
 *   setters are -setFlag:forSetting:, -setObject:forSetting:,
 *   -setInteger:forSetting:, -setDouble:forSetting:, -setSize:,
 *   -setRect:, -setPoint:, -setEdgeInsets:, -setFrame:
 *   [src: https://developer.limneos.net/?ios=18.1&framework=BaseBoard.framework&header=BSSettings.h].
 *   The BSXPC coder's on-the-wire key strings are NOT in any
 *   header. The working
 *   hypothesis in this file is that a BSSettings encodes as a plain
 *   XPC dictionary whose keys are the setting property names as
 *   strings [guess]; the alternative hypothesis is small integer
 *   setting IDs [guess]. Only a device trace can settle it.
 * - BSAction properties are info, handler, kind, listener,
 *   responder, target, timeout; methods -sendResponse:,
 *   -respondWithError:, -canSendResponse
 *   [src: https://developer.limneos.net/?ios=18.1&framework=BaseBoard.framework&header=BSAction.h].
 *   Their use as wire keys is [guess].
 * - RBSProcessHandle properties are identity, pid, auid, rbs_pid,
 *   auditToken, isValid, isRunning, isSuspended, isForeground,
 *   isSystemApplication, isApplication, isDaemon
 *   [src: https://developer.limneos.net/?ios=18.1&framework=RunningBoardServices.framework&header=RBSProcessHandle.h].
 *   Their use as wire keys is [guess].
 * - The BSXPC message path (BSXPCMessage
 *   -initWithMessagePacker:replyHandler:replyQueue: and
 *   +sendMessageWithPacker:toConnection:replyHandler:replyQueue:,
 *   BSBaseXPCClient
 *   -_sendMessage:withReplyHandler:waitForReply:waitDuration:,
 *   BSSerializeBSXPCEncodableObjectToXPCDictionary and
 *   BSCreateSerializedBSXPCEncodableObject) is attested by a public
 *   crash log
 *   [src: https://iosgods.com/topic/6413-hack-how-to-get-action-movie-fx-videos-effects-for-free/page/5/].
 *   The device trace (2026-11-12) measured the real envelope keys
 *   ("bsxpc", "bsxpc_context", "bsxpc_CID", "bsxpc_CIDr",
 *   "bsxpc_SEL").
 * - Size/rect/point/insets encode as nested dictionaries
 *   {width, height} / {x, y, width, height} / {x, y} /
 *   {top, left, bottom, right}, all doubles [guess]. There is no
 *   public source for the geometric encoding; it is the simplest
 *   shape consistent with BSSettings -setSize:/-setRect: and with
 *   the XPC types available.
 * - Audit token as 32-byte XPC data, 8 little-endian u32 words in
 *   audit_token_t order (val[0] auid, val[1] euid, val[2] egid,
 *   val[3] ruid, val[4] rgid, val[5] pid, val[6] asid, val[7]
 *   pidversion) [guess] for the encoding. The field order itself
 *   is measured. The forge shape (all zero except pid and pidversion 1)
 *   is the launchd-reply forgery shape; using it for a RunningBoard
 *   handle is [guess].
 * - There is no reply bit or key in the XPC payload; reply-ness is
 *   msgh_id 0x20000000 plus the send-once reply port.
 *
 * No em dashes are used in this file.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "xpcwire.h"

/* ------------------------------------------------------------------ */
/* BSSettings: typed key/value store on top of an XPC dictionary.      */
/* ------------------------------------------------------------------ */

/* Open a BSSettings dictionary (xw_dict_begin) and close it
 * (xw_dict_end). The caller owns the builder and must have called
 * xw_builder_init first. Setters below append to the innermost
 * dictionary, so they must be called between begin and end.
 */
int bs_settings_begin(xw_builder *b);
int bs_settings_end(xw_builder *b);

/* Setters. key is the wire key string, supplied by the caller (see
 * the file header: the fakeboard names table owns the spellings).
 * All return XW_OK or a negative XW_ERR_*; the builder keeps its
 * sticky error, so callers may chain them with if (r == XW_OK).
 */
int bs_settings_set_flag(xw_builder *b, const char *key, int value);
int bs_settings_set_int(xw_builder *b, const char *key, int64_t value);
int bs_settings_set_uint(xw_builder *b, const char *key, uint64_t value);
int bs_settings_set_double(xw_builder *b, const char *key, double value);
int bs_settings_set_string(xw_builder *b, const char *key, const char *value);
int bs_settings_set_data(xw_builder *b, const char *key,
                         const void *ptr, uint32_t len);
/* Geometric values encode as nested dictionaries, all doubles
 * [guess] (see the file header): size {width, height}, rect
 * {x, y, width, height}, point {x, y}, insets {top, left, bottom,
 * right}. bs_settings_set_frame is identical to set_rect.
 */
int bs_settings_set_size(xw_builder *b, const char *key,
                         double width, double height);
int bs_settings_set_rect(xw_builder *b, const char *key,
                         double x, double y, double width, double height);
int bs_settings_set_point(xw_builder *b, const char *key,
                          double x, double y);
int bs_settings_set_insets(xw_builder *b, const char *key,
                           double top, double left,
                           double bottom, double right);
int bs_settings_set_frame(xw_builder *b, const char *key,
                          double x, double y, double width, double height);

/* Readers. r is the reader for the message (non-const: xw_dict_find
 * takes a non-const xw_reader). key is the wire key string. Each
 * returns XW_OK or a negative XW_ERR_* (XW_ERR_NOT_FOUND when the
 * key is absent, XW_ERR_BAD_TYPE when the value has the wrong type
 * or is not a dictionary for the geometric getters). out pointers
 * must be non-NULL. get_string returns a pointer into the reader
 * buffer (the string bytes, NUL terminated) plus its length
 * including the NUL in *len_out when len_out is non-NULL.
 */
int bs_settings_get_flag(xw_reader *r, const char *key, int *out);
int bs_settings_get_int(xw_reader *r, const char *key, int64_t *out);
int bs_settings_get_uint(xw_reader *r, const char *key, uint64_t *out);
int bs_settings_get_double(xw_reader *r, const char *key, double *out);
int bs_settings_get_string(xw_reader *r, const char *key,
                           const char **out, uint32_t *len_out);
int bs_settings_get_size(xw_reader *r, const char *key,
                         double *width_out, double *height_out);
int bs_settings_get_rect(xw_reader *r, const char *key,
                         double *x_out, double *y_out,
                         double *width_out, double *height_out);
int bs_settings_get_point(xw_reader *r, const char *key,
                          double *x_out, double *y_out);
int bs_settings_get_insets(xw_reader *r, const char *key,
                           double *top_out, double *left_out,
                           double *bottom_out, double *right_out);

/* ------------------------------------------------------------------ */
/* BSAction: scene-action carrier.                                     */
/* ------------------------------------------------------------------ */

/* Wire keys for an action dictionary. The key spellings are [guess]
 * (see the file header); the fakeboard names table supplies them.
 */
typedef struct {
    const char *key_kind;     /* e.g. "kind" [guess] */
    const char *key_info;     /* e.g. "info" [guess] */
    const char *key_target;   /* e.g. "target" [guess] */
    const char *key_timeout;  /* e.g. "timeout" [guess] */
    const char *key_response; /* e.g. "response" [guess] */
} bsxpc_action_keys;

/* Open an action dictionary and write its fixed fields: kind and
 * target as strings, timeout_ms as uint64. The dictionary is left
 * OPEN on purpose: the caller adds the optional info nested
 * dictionary (xw_dict_add_dict_begin with keys->key_info, then the
 * info fields, then xw_dict_end) and closes the action dictionary
 * with xw_dict_end. If no info is needed, the caller just calls
 * xw_dict_end once. Returns XW_OK or a negative XW_ERR_*.
 */
int bsxpc_action_build(xw_builder *b, const bsxpc_action_keys *keys,
                       const char *kind, const char *target,
                       uint64_t timeout_ms);

/* Parse an action dictionary. kind_buf and target_buf are filled
 * with NUL-terminated copies (including the NUL in the cap check:
 * returns XW_ERR_NOSPACE when the string does not fit). timeout_out
 * receives the uint64 timeout. The info dictionary, when present,
 * is ignored by this function; callers read it with
 * xw_dict_find(r, keys->key_info, ...) themselves. Returns XW_OK
 * or a negative XW_ERR_* (XW_ERR_NOT_FOUND when kind, target or
 * timeout is absent).
 */
int bsxpc_action_parse(xw_reader *r, const bsxpc_action_keys *keys,
                       char *kind_buf, size_t kind_cap,
                       char *target_buf, size_t target_cap,
                       uint64_t *timeout_out);

/* ------------------------------------------------------------------ */
/* BSProcessHandle / RBSProcessHandle.                                 */
/* ------------------------------------------------------------------ */

/* Wire keys for a process-handle dictionary. Key spellings are
 * [guess] (see the file header); the fakeboard names table supplies
 * them. */
typedef struct {
    const char *key_pid;           /* e.g. "pid" [guess] */
    const char *key_bundle_id;     /* e.g. "bundleID" [guess] */
    const char *key_audit_token;   /* e.g. "auditToken" [guess] */
    const char *key_is_running;    /* e.g. "isRunning" [guess] */
    const char *key_is_foreground; /* e.g. "isForeground" [guess] */
    const char *key_is_valid;      /* e.g. "isValid" [guess] */
    const char *key_is_application;/* e.g. "isApplication" [guess] */
} bsxpc_handle_keys;

/* Audit token, 8 u32 words in audit_token_t order. See the file
 * header for the field order and the forge shape.
 */
typedef struct {
    uint32_t auid;
    uint32_t euid;
    uint32_t egid;
    uint32_t ruid;
    uint32_t rgid;
    uint32_t pid;
    uint32_t asid;
    uint32_t pidversion;
} bsxpc_audit_token;

typedef struct {
    uint32_t pid;
    const char *bundle_id; /* NULL on build means: omit the key */
    bsxpc_audit_token audit;
    int is_running;
    int is_foreground;
    int is_valid;
    int is_application;
} bsxpc_process_handle;

/* Write a complete process-handle dictionary (begin, fields, end).
 * pid is a uint64, bundle_id a string (omitted when NULL),
 * audit_token a 32-byte data value (see bsxpc_add_audit_token),
 * the four flags are bools. Returns XW_OK or a negative XW_ERR_*.
 */
int bsxpc_handle_build(xw_builder *b, const bsxpc_handle_keys *keys,
                       const bsxpc_process_handle *h);

/* Parse a process-handle dictionary. bundle_id is copied into
 * bundle_buf (NUL terminated; XW_ERR_NOSPACE when it does not fit)
 * and out->bundle_id is set to bundle_buf on success. All fields
 * are required; a missing key returns XW_ERR_NOT_FOUND. Returns
 * XW_OK or a negative XW_ERR_*.
 */
int bsxpc_handle_parse(xw_reader *r, const bsxpc_handle_keys *keys,
                       bsxpc_process_handle *out,
                       char *bundle_buf, size_t bundle_cap);

/* ------------------------------------------------------------------ */
/* Audit tokens.                                                       */
/* ------------------------------------------------------------------ */

/* Fill tok with the launchd-reply forgery shape: all zero except
 * pid (the argument) and pidversion 1; using it for a RunningBoard
 * handle is [guess].
 */
int bsxpc_audit_token_forge(bsxpc_audit_token *tok, uint32_t pid);

/* Add an audit token under key as a 32-byte XPC data value (8
 * little-endian u32 words, audit_token_t order). The 32-byte data
 * encoding is [guess] (see the file header). The innermost builder
 * container must be a dictionary. Returns XW_OK or a negative
 * XW_ERR_*.
 */
int bsxpc_add_audit_token(xw_builder *b, const char *key,
                          const bsxpc_audit_token *tok);

/* Read an audit token stored under key. The value must be 32 bytes
 * of inline data, else XW_ERR_BAD_TYPE. Returns XW_OK or a
 * negative XW_ERR_* (XW_ERR_NOT_FOUND when the key is absent).
 */
int bsxpc_get_audit_token(xw_reader *r, const char *key,
                          bsxpc_audit_token *out);

/* ------------------------------------------------------------------ */
/* BSXPCServiceConnection message envelope (real keys).     */
/* ------------------------------------------------------------------ */

/* Wire keys for the envelope dictionary. The key spellings are
 * measured on device 2026-11-12; the fakeboard names table supplies them.
 * There is NO default keys struct: the names are parameters
 * precisely because they are unverified.
 */
typedef struct {
    const char *key_bsxpc;    /* e.g. "bsxpc" */
    const char *key_context;  /* e.g. "bsxpc_context" */
    const char *key_cid;      /* e.g. "bsxpc_CID" */
    const char *key_cidr;     /* e.g. "bsxpc_CIDr" */
    const char *key_sel;      /* e.g. "bsxpc_SEL" */
} bsxpc_envelope_keys;

/* A parsed BSXPC message. kind/sel point into the reader buffer
 * (NUL-terminated strings) and are NULL when the key is absent;
 * cid is the call id (0 when absent); cidr is the reply flag
 * (0 when absent). ctx is the "bsxpc_context" dict value; has_ctx
 * is 1 when it holds that dict. Callers read parameters from ctx
 * with xw_dict_find_v / xw_dict_iter_init_v (the value carries its
 * own descs, so port values inside the context resolve correctly).
 * The has_* flags say which keys were present; a well-formed
 * message may legitimately omit any of them, so parse succeeds
 * with all flags clear on an empty dict.
 */
typedef struct {
    const char *kind;   /* "bsxpc" string value, or NULL */
    const char *sel;    /* "bsxpc_SEL" string value, or NULL */
    uint64_t cid;       /* "bsxpc_CID" uint64 value, 0 if absent */
    int cidr;           /* "bsxpc_CIDr" bool value, 0 if absent */
    int has_kind;
    int has_sel;
    int has_cid;
    int has_cidr;
    xw_value ctx;       /* "bsxpc_context" dict value */
    int has_ctx;
} bsxpc_message;

/* Parse an incoming BSXPC message dict: extract the bsxpc kind
 * (string), bsxpc_SEL (string), bsxpc_CID (uint64), bsxpc_CIDr
 * (bool), and the bsxpc_context dict into out->ctx. A key that is
 * present but has the wrong type returns XW_ERR_BAD_TYPE; a key
 * that is absent clears its has_* flag and leaves the field at its
 * zero value. A root that is not a dictionary returns
 * XW_ERR_BAD_TYPE. Returns XW_OK or a negative XW_ERR_*.
 */
int bsxpc_message_parse(xw_reader *r, const bsxpc_envelope_keys *keys,
                        bsxpc_message *out);

/* Build a completion reply dict for a given CID: open a dict, write
 * "bsxpc_CID" (the same CID) and "bsxpc_CIDr" (true), and leave the
 * dict OPEN for the caller to add result fields, then close it with
 * xw_dict_end. The reply msgh_id 0x20000000 is set by the caller in
 * the Mach header, not by bsxpc. Returns XW_OK or a negative
 * XW_ERR_*.
 */
int bsxpc_reply_build(xw_builder *b, const bsxpc_envelope_keys *keys,
                      uint64_t cid);

/* ------------------------------------------------------------------ */
/* Verbatim trace logger.                           */
/* ------------------------------------------------------------------ */

/* True (non-zero) when tracing is enabled. Reads the environment
 * once via lsk_getenv("IOSWINE_SVC_TRACE") and caches the result;
 * enabled only when the value is exactly "1". A test override set
 * with bsxpc_set_trace takes precedence over the environment.
 * On the host the test provides lsk_getenv (reading the real
 * process environment); on target init.c provides it.
 */
int bsxpc_trace_enabled(void);

/* Test override for tracing. on > 0 forces tracing on, on == 0
 * forces it off, on < 0 clears the override AND the cached
 * environment value, so the next bsxpc_trace_enabled call re-reads
 * the environment. The override takes precedence over the
 * environment while it is set.
 */
void bsxpc_set_trace(int on);

/* Log the whole message verbatim to fd 2, one line per entry:
 *   <prefix> <indent>"key": type = value
 * with 2 spaces of indent per nesting depth, strings quoted
 * (with " and \ escaped, control bytes as \xHH), data as the
 * first 16 bytes in hex plus its length, doubles in %f style
 * with 6 fractional digits, ports as desc#N disp=0x..,
 * nested containers recursed with an opening "{", the children,
 * and a closing "}" line. prefix is written verbatim at the start
 * of every line (pass "[fakeboard]" or similar, or NULL for no
 * prefix). When tracing is off this returns 0 silently and writes
 * nothing. When the reader is invalid it prints "<malformed>" and
 * returns a negative XW_ERR_* without crashing and without reading
 * past the buffer. The caller must have run the reader through
 * xw_reader_init first (success or failure) and must have
 * zero-initialized the reader struct before init, so a failed init
 * leaves a defined state; a reader that never went through init is
 * undefined behaviour, as it is for every other xpcwire consumer.
 * Returns 0 on success, a negative XW_ERR_* on a malformed
 * reader, or 0 silently when tracing is off.
 */
int bsxpc_log_dict(const xw_reader *r, const char *prefix);
