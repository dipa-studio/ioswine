/*
 * fakerb.c: fake RunningBoard service for ioswine.
 *
 * Answers the RBSConnection side of the launch handshake for the app
 * process itself. The fake owns no daemon and no separate process:
 * the app process is the only process, so every handle, assertion
 * and state report describes it.
 *
 * Dispatch is keyed by bsxpc_SEL (see bsxpc_message_parse in bsxpc.h),
 * and every reply echoes the request CID with CIDr=true when
 * the request carried one (see bsxpc_reply_build, via
 * rb_reply_begin). Arguments are looked up in bsxpc_context first,
 * then at the message root: the parameters moved into the context
 * in the real envelope. The connection handshake
 * (handshakeWithRequest:, measured on device 2026-11-12: Aidoku
 * sends RBS handshakeWithRequest: to the fake) is answered with the
 * process handle dict plus error 0; the exact reply shape is
 * [guess] (see rb_handshake).
 *
 * The "connect" reply rule applies here too: a message whose
 * msgh_id is 0x10000000 is one-way and gets NO reply, even when it
 * carries a reply port; a message with msgh_id 0x40000000 expects
 * a reply, and gets one. The rule is enforced in fakerb_process_msg
 * (via rb_dispatch setting ctx->no_reply). The svchost adapter path
 * never sees a msgh_id, so it cannot enforce the rule. See
 * fakerb.h for the full note and the live-path gap.
 *
 * Execution model: synchronous in the sender's thread as a kobject,
 * exactly like svchost.c. Two entry
 * points share one dispatch function:
 *   - rb_svchost_handler: the svchost handler registered by
 *     fakerb_init; svchost_process_msg owns the reply header and
 *     the never-drop-a-reply rule for this path.
 *   - fakerb_process_msg: a standalone kobject entry with the same
 *     shape as svchost_process_msg, for a dedicated ipc.c dispatch
 *     branch or for direct use by the host simulator.
 * Handlers are pure dict transforms. They run under ipc_lock held by
 * the dispatching thread, so they never call anything that takes
 * ipc_lock (no ipc_msg, no port allocate/destroy, no mach_msg), never
 * block, and never allocate. All state lives in the static tables
 * below.
 *
 * bsxpc usage note: the bs_settings_set_* helpers are used here as
 * field fillers for a dict the caller has already opened
 * (xw_dict_add_dict_begin), matching how the setters take a builder
 * plus a key. bsxpc_handle_build is NOT used that way: it writes a
 * complete standalone root dict, so rb_add_handle_fields writes the
 * handle fields one by one under the open dict instead (see there;
 * the simulator caught the mismatch on 2026-11-12). The audit
 * token in every handle is forged for the self pid (auid/euid 0,
 * pidversion 1, as the launchd shim already does for its trailer).
 *
 * Style: freestanding C, same as launchd.c / bks.c / svchost.c. No
 * libc. No unaligned reads; every read goes through the xpcwire
 * reader, which bounds-checks it. No wire-string literals: every
 * string comes from fakeboard_names.h (via the FAKERB_* aliases in
 * fakerb.h) or from bsxpc.h.
 */
#include "fakerb.h"
#include "svchost.h"
#include "bsxpc.h"
#include "linux.h"

/* Freestanding string helper. */
static int rb_strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

/* ------------------------------------------------------------------ */
/* Static state. No allocation; tables are fixed size.                 */
/* ------------------------------------------------------------------ */

static uint32_t g_rb_self_pid = 0;
static char g_rb_self_bundle_id[FAKERB_BUNDLE_ID_MAX] =
    FAKERB_DEFAULT_BUNDLE_ID;
static uint32_t g_rb_next_id = 1; /* monotonic id counter, never 0 */
static uint32_t g_rb_assertions[FAKERB_MAX_ASSERTIONS];
static uint32_t g_rb_assertion_count = 0;
static uint32_t g_rb_monitors[FAKERB_MAX_MONITORS];
static uint32_t g_rb_monitor_count = 0;
static int g_rb_inited = 0;

/* The BSXPC envelope keys, in bsxpc_envelope_keys field order. */
static const bsxpc_envelope_keys rb_env_keys = {
    .key_bsxpc = FAKERB_KEY_BSXPC,
    .key_context = FAKERB_KEY_CONTEXT,
    .key_cid = FAKERB_KEY_CID,
    .key_cidr = FAKERB_KEY_CIDR,
    .key_sel = FAKERB_KEY_SEL,
};

/* The handle key table, in bsxpc_handle_keys field order. */
static const bsxpc_handle_keys rb_handle_keys = {
    .key_pid = FAKERB_HANDLE_KEY_PID,
    .key_bundle_id = FAKERB_HANDLE_KEY_BUNDLE_ID,
    .key_audit_token = FAKERB_HANDLE_KEY_AUDIT_TOKEN,
    .key_is_running = FAKERB_HANDLE_KEY_IS_RUNNING,
    .key_is_foreground = FAKERB_HANDLE_KEY_IS_FOREGROUND,
    .key_is_valid = FAKERB_HANDLE_KEY_IS_VALID,
    .key_is_application = FAKERB_HANDLE_KEY_IS_APPLICATION,
};

/* Hand out the next id, skipping 0 (ids wrap only after ~4 billion
 * acquires, so a collision with a live id after a wrap is not a
 * practical concern in v1). */
static uint32_t rb_next_id(void) {
    uint32_t id = g_rb_next_id++;
    if (id == 0) id = g_rb_next_id++;
    return id;
}

static int rb_assertion_remove(uint32_t id) {
    uint32_t i;
    for (i = 0; i < g_rb_assertion_count; i++) {
        if (g_rb_assertions[i] == id) {
            g_rb_assertion_count--;
            g_rb_assertions[i] = g_rb_assertions[g_rb_assertion_count];
            return 1;
        }
    }
    return 0;
}

static int rb_monitor_remove(uint32_t id) {
    uint32_t i;
    for (i = 0; i < g_rb_monitor_count; i++) {
        if (g_rb_monitors[i] == id) {
            g_rb_monitor_count--;
            g_rb_monitors[i] = g_rb_monitors[g_rb_monitor_count];
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Reply framing: minimal {"error": code} body, reply header, CID    */
/* echo.                                                               */
/* ------------------------------------------------------------------ */

/* Open a reply dict in the builder: when the request carried a CID,
 * write bsxpc_CID (same CID) and bsxpc_CIDr (true) via
 * bsxpc_reply_build (the dict is left OPEN for the caller to add
 * result fields and close with xw_dict_end); when it did not, open a
 * plain dict. The reply msgh_id (0x20000000) is set by the caller in
 * the Mach header, not here. Returns XW_OK or a negative XW_ERR_*. */
static int rb_reply_begin(xw_builder *b, const bsxpc_message *m) {
    if (m != NULL && m->has_cid) {
        return bsxpc_reply_build(b, &rb_env_keys, m->cid);
    }
    return xw_dict_begin(b);
}

static int rb_build_error_body(uint8_t *body, size_t cap, int64_t code,
                               uint32_t *out_len) {
    xw_builder b;
    if (xw_builder_init(&b, body, cap) != XW_OK) return -1;
    if (xw_dict_begin(&b) != XW_OK) return -1;
    if (xw_dict_add_int64(&b, FAKERB_KEY_ERROR, code) != XW_OK) return -1;
    if (xw_dict_end(&b) != XW_OK) return -1;
    *out_len = (uint32_t)xw_builder_len(&b);
    return 0;
}

/* Reply header: msgh_id
 * 0x20000000, bits remote MOVE_SEND_ONCE / local 0 / voucher NULL,
 * remote = the request's reply port. The trailer (CTX msgh_context,
 * seqno, audit pid 1 / euid 0) is ipc.c's domain when it enqueues
 * this reply, not fakerb.c's. */
static void rb_fill_reply_header(uint8_t *reply_buf, uint32_t body_len,
                                 mach_port_name_t reply_port) {
    mach_msg_header_t *rep = (mach_msg_header_t *)reply_buf;
    rep->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
    rep->msgh_size = sizeof(mach_msg_header_t) + body_len;
    rep->msgh_remote_port = reply_port;
    rep->msgh_local_port = MACH_PORT_NULL;
    rep->msgh_voucher_port = MACH_PORT_NULL;
    rep->msgh_id = SVCHOST_REPLY_MSGH_ID;
}

/* ------------------------------------------------------------------ */
/* Handle and state builders.                                          */
/* ------------------------------------------------------------------ */

/* Fill a bsxpc_process_handle for the app process itself. */
static void rb_fill_self_handle(bsxpc_process_handle *h) {
    bsxpc_audit_token_forge(&h->audit, g_rb_self_pid);
    h->pid = g_rb_self_pid;
    h->bundle_id = g_rb_self_bundle_id;
    h->is_running = 1;
    h->is_foreground = 1;
    h->is_valid = 1;
    h->is_application = 1;
}

/* Add the handle fields into the innermost (already open) dict.
 * Caller opens the dict with xw_dict_add_dict_begin and closes it
 * with xw_dict_end. The fields are written one by one, using the
 * same rb_handle_keys table bsxpc_handle_build uses, so the two
 * shapes stay consistent. bsxpc_handle_build itself cannot be used
 * here: it writes a complete standalone root dict, and calling it
 * inside an already-open dict returns XW_ERR_BAD_TYPE and poisons
 * the builder (found by the host simulator on 2026-11-12: every
 * handle reply came back {"error": 12}). */
static int rb_add_handle_fields(xw_builder *b) {
    bsxpc_process_handle h;
    int r = XW_OK;
    rb_fill_self_handle(&h);
    if (r == XW_OK)
        r = xw_dict_add_uint64(b, rb_handle_keys.key_pid, h.pid);
    if (r == XW_OK && h.bundle_id != NULL)
        r = xw_dict_add_string(b, rb_handle_keys.key_bundle_id,
                               h.bundle_id);
    if (r == XW_OK)
        r = bsxpc_add_audit_token(b, rb_handle_keys.key_audit_token,
                                  &h.audit);
    if (r == XW_OK)
        r = xw_dict_add_bool(b, rb_handle_keys.key_is_running,
                             h.is_running);
    if (r == XW_OK)
        r = xw_dict_add_bool(b, rb_handle_keys.key_is_foreground,
                             h.is_foreground);
    if (r == XW_OK)
        r = xw_dict_add_bool(b, rb_handle_keys.key_is_valid, h.is_valid);
    if (r == XW_OK)
        r = xw_dict_add_bool(b, rb_handle_keys.key_is_application,
                             h.is_application);
    return r;
}

/* Add the process-state fields into the innermost (already open)
 * dict: the handle for self, taskState running, foreground, running,
 * not suspended, not terminating, and the visibility endowment
 * namespace the WebKit foreground check looks for. No asynchronous
 * push in v1: the subscription reply below carries this state once,
 * and the fake never pushes later changes. */
static int rb_add_state_fields(xw_builder *b) {
    int r = XW_OK;
    if (r == XW_OK) r = xw_dict_add_dict_begin(b, FAKERB_KEY_HANDLE);
    if (r == XW_OK) r = rb_add_handle_fields(b);
    if (r == XW_OK) r = xw_dict_end(b);
    if (r == XW_OK) r = bs_settings_set_uint(b, FAKERB_STATE_KEY_TASK_STATE,
                                             FAKERB_TASK_STATE_RUNNING_SCHEDULED);
    if (r == XW_OK) r = bs_settings_set_flag(b, FAKERB_STATE_KEY_IS_FOREGROUND, 1);
    if (r == XW_OK) r = bs_settings_set_flag(b, FAKERB_STATE_KEY_IS_RUNNING, 1);
    if (r == XW_OK) r = bs_settings_set_flag(b, FAKERB_STATE_KEY_IS_SUSPENDED, 0);
    if (r == XW_OK) r = bs_settings_set_flag(b, FAKERB_STATE_KEY_IS_TERMINATING, 0);
    if (r == XW_OK) r = bs_settings_set_int(b, FAKERB_STATE_KEY_LAST_EXIT_STATUS, 0);
    if (r == XW_OK) r = bs_settings_set_int(b, FAKERB_STATE_KEY_TERMINATION_REASON, 0);
    if (r == XW_OK) r = xw_dict_add_array_begin(b, FAKERB_STATE_KEY_ENDOWMENTS);
    if (r == XW_OK) r = xw_add_string(b, FAKERB_ENDOWMENT_VISIBILITY);
    if (r == XW_OK) r = xw_array_end(b);
    return r;
}

/* Build {"error": 0, "handle": {...self...}} into reply, echoing the
 * request CID with CIDr=true when it carried one. Returns 0 or
 * SVCHOST_ERR_NO_MEMORY if the reply did not fit. */
static int rb_reply_handle(const bsxpc_message *m, xw_builder *reply) {
    int r = rb_reply_begin(reply, m);
    if (r == XW_OK) r = xw_dict_add_int64(reply, FAKERB_KEY_ERROR, 0);
    if (r == XW_OK) r = xw_dict_add_dict_begin(reply, FAKERB_KEY_HANDLE);
    if (r == XW_OK) r = rb_add_handle_fields(reply);
    if (r == XW_OK) r = xw_dict_end(reply);
    if (r == XW_OK) r = xw_dict_end(reply);
    return (r == XW_OK) ? 0 : SVCHOST_ERR_NO_MEMORY;
}

/* Build {"error": 0} into reply, echoing the request CID with
 * CIDr=true when it carried one. Returns 0 or
 * SVCHOST_ERR_NO_MEMORY if the reply did not fit. */
static int rb_reply_ok(const bsxpc_message *m, xw_builder *reply) {
    int r = rb_reply_begin(reply, m);
    if (r == XW_OK) r = xw_dict_add_int64(reply, FAKERB_KEY_ERROR, 0);
    if (r == XW_OK) r = xw_dict_end(reply);
    return (r == XW_OK) ? 0 : SVCHOST_ERR_NO_MEMORY;
}

/* ------------------------------------------------------------------ */
/* Argument lookup. Arguments live in bsxpc_context in the real      */
/* envelope; a top-level key of the same name is accepted as a        */
/* fallback, in case the real encoding is flat rather than nested.   */
/* ------------------------------------------------------------------ */

static int rb_find_arg(const bsxpc_message *m, xw_reader *msg,
                       const char *key, xw_value *out) {
    if (m != NULL && m->has_ctx) {
        if (xw_dict_find_v(&m->ctx, key, out) == XW_OK) return XW_OK;
    }
    return xw_dict_find(msg, key, out);
}

static int rb_find_arg_uint64(const bsxpc_message *m, xw_reader *msg,
                              const char *key, uint64_t *out) {
    xw_value v;
    if (rb_find_arg(m, msg, key, &v) != XW_OK) return XW_ERR_NOT_FOUND;
    return xw_as_uint64(&v, out);
}

/* ------------------------------------------------------------------ */
/* Message handlers. Each returns 0 when the reply dict was built    */
/* in reply, or an error code the dispatcher turns into               */
/* {"error": code}. m is the parsed BSXPC envelope (CID echo source  */
/* and context lookup); msg is the reader for root-level fallback.   */
/* ------------------------------------------------------------------ */

static int rb_handle_for_predicate(const bsxpc_message *m, xw_reader *msg,
                                   xw_builder *reply) {
    /* The predicate (bundle id, job label, ...) is ignored: in
     * ioswine there is only the app process, so any predicate that
     * reaches us is answered with the self handle. */
    (void)msg;
    if (g_rb_self_pid == 0) return SVCHOST_ERR_UNKNOWN_MESSAGE;
    return rb_reply_handle(m, reply);
}

static int rb_handle_for_pid(const bsxpc_message *m, xw_reader *msg,
                             xw_builder *reply) {
    uint64_t pid = 0;
    if (rb_find_arg_uint64(m, msg, FAKERB_KEY_PID, &pid) != XW_OK)
        return SVCHOST_ERR_BAD_MESSAGE;
    if (g_rb_self_pid == 0 || pid != (uint64_t)g_rb_self_pid)
        return SVCHOST_ERR_UNKNOWN_MESSAGE;
    return rb_reply_handle(m, reply);
}

static int rb_states_for_predicate(const bsxpc_message *m, xw_reader *msg,
                                   xw_builder *reply) {
    (void)msg;
    if (g_rb_self_pid == 0) return SVCHOST_ERR_UNKNOWN_MESSAGE;
    {
        int r = rb_reply_begin(reply, m);
        if (r == XW_OK) r = xw_dict_add_int64(reply, FAKERB_KEY_ERROR, 0);
        if (r == XW_OK) r = xw_dict_add_array_begin(reply, FAKERB_KEY_STATES);
        if (r == XW_OK) r = xw_dict_begin(reply);
        if (r == XW_OK) r = rb_add_state_fields(reply);
        if (r == XW_OK) r = xw_dict_end(reply);
        if (r == XW_OK) r = xw_array_end(reply);
        if (r == XW_OK) r = xw_dict_end(reply);
        return (r == XW_OK) ? 0 : SVCHOST_ERR_NO_MEMORY;
    }
}

static int rb_acquire_assertion(const bsxpc_message *m, xw_reader *msg,
                                xw_builder *reply) {
    /* Target, attributes and explanation arguments are accepted and
     * ignored; the assertion descriptor format is [guess] and the
     * fake records only the id. */
    uint32_t id;
    (void)m;
    (void)msg;
    if (g_rb_assertion_count >= FAKERB_MAX_ASSERTIONS)
        return SVCHOST_ERR_NO_MEMORY;
    id = rb_next_id();
    g_rb_assertions[g_rb_assertion_count++] = id;
    {
        int r = rb_reply_begin(reply, m);
        if (r == XW_OK) r = xw_dict_add_int64(reply, FAKERB_KEY_ERROR, 0);
        if (r == XW_OK)
            r = xw_dict_add_uint64(reply, FAKERB_KEY_IDENTIFIER, id);
        if (r == XW_OK) r = xw_dict_end(reply);
        return (r == XW_OK) ? 0 : SVCHOST_ERR_NO_MEMORY;
    }
}

static int rb_invalidate_assertion(const bsxpc_message *m, xw_reader *msg,
                                   xw_builder *reply) {
    uint64_t id = 0;
    if (rb_find_arg_uint64(m, msg, FAKERB_KEY_IDENTIFIER, &id) != XW_OK)
        return SVCHOST_ERR_BAD_MESSAGE;
    /* Idempotent in effect: after this call the id is not held,
     * whether it was held before or not. An id that was never held
     * (or was already invalidated) still answers, with {"error": 1},
     * so the client learns it is gone. */
    if (!rb_assertion_remove((uint32_t)id))
        return SVCHOST_ERR_UNKNOWN_MESSAGE;
    return rb_reply_ok(m, reply);
}

static int rb_subscribe_state(const bsxpc_message *m, xw_reader *msg,
                              xw_builder *reply) {
    uint32_t id;
    (void)m;
    (void)msg;
    if (g_rb_monitor_count >= FAKERB_MAX_MONITORS)
        return SVCHOST_ERR_NO_MEMORY;
    id = rb_next_id();
    g_rb_monitors[g_rb_monitor_count++] = id;
    /* The reply carries the current state at once: running and
     * foreground. A real daemon would push later changes over the
     * connection; this fake never pushes. */
    {
        int r = rb_reply_begin(reply, m);
        if (r == XW_OK) r = xw_dict_add_int64(reply, FAKERB_KEY_ERROR, 0);
        if (r == XW_OK)
            r = xw_dict_add_uint64(reply, FAKERB_KEY_IDENTIFIER, id);
        if (r == XW_OK) r = xw_dict_add_dict_begin(reply, FAKERB_KEY_STATE);
        if (r == XW_OK) r = rb_add_state_fields(reply);
        if (r == XW_OK) r = xw_dict_end(reply);
        if (r == XW_OK) r = xw_dict_end(reply);
        return (r == XW_OK) ? 0 : SVCHOST_ERR_NO_MEMORY;
    }
}

static int rb_unsubscribe_state(const bsxpc_message *m, xw_reader *msg,
                                xw_builder *reply) {
    uint64_t id = 0;
    if (rb_find_arg_uint64(m, msg, FAKERB_KEY_IDENTIFIER, &id) != XW_OK)
        return SVCHOST_ERR_BAD_MESSAGE;
    if (!rb_monitor_remove((uint32_t)id))
        return SVCHOST_ERR_UNKNOWN_MESSAGE;
    return rb_reply_ok(m, reply);
}

/* handshakeWithRequest: (device 2026-11-12: Aidoku sends RBS
 * handshakeWithRequest: to the fake, with arguments
 * RBSHandshakeRequest, _assertionDescriptors, _savedEndowments).
 * The arguments are accepted, not validated, in v1: the fake has
 * nothing to negotiate, and the client only needs its process
 * handle back to proceed. The reply is the handle dict plus error
 * 0, with the CID echo (CIDr=true) when the request carried one.
 * The exact reply shape is [guess]: whether the real daemon returns
 * the handle under "handle", or a richer handshake response (state,
 * endowments, a handshake result object), is not measured yet; the
 * handle dict + error 0 is the minimal shape the client can use.
 * The three argument key names are not in fakeboard_names.h yet
 * (see the report); if a later change validates them, add them to
 * the table and read them here with rb_find_arg. */
static int rb_handshake(const bsxpc_message *m, xw_reader *msg,
                        xw_builder *reply) {
    (void)msg;
    if (g_rb_self_pid == 0) return SVCHOST_ERR_UNKNOWN_MESSAGE;
    return rb_reply_handle(m, reply);
}

/* Known message, unsupported in v1: launch and terminate requests,
 * process-death subscriptions, endowment saves. Answered, never
 * silent, so the client does not block on a dropped reply. */
static int rb_unsupported(const bsxpc_message *m, xw_reader *msg,
                          xw_builder *reply) {
    (void)m;
    (void)msg;
    (void)reply;
    return SVCHOST_ERR_UNKNOWN_MESSAGE;
}

/* ------------------------------------------------------------------ */
/* Dispatch. The bsxpc_SEL is the only match key, so a corrected     */
/* selector after verification is a one-line change in               */
/* fakeboard_names.h. No function-pointer table: arm64e signed       */
/* pointers in static data are not linkable with our ld64.lld        */
/* setup.                                                            */
/* ------------------------------------------------------------------ */

/* Per-request context. msgh_id / have_msgh_id come from the Mach
 * header (kobject path only); no_reply is set by rb_dispatch when
 * the msgh_id rule fires, and the caller sends no Mach reply. */
typedef struct {
    uint32_t msgh_id;   /* raw header msgh_id, when have_msgh_id */
    int have_msgh_id;   /* the caller supplied a msgh_id */
    int no_reply;       /* OUT: the msgh_id rule says send no reply */
} rb_req_ctx;

/* Dispatch one parsed message. Returns 0 when the reply dict was
 * built in reply, or an error code for {"error": code}. A message
 * with no bsxpc_SEL, and any selector not in the table, answers
 * {"error": 1}. When ctx->no_reply is set, the return value and the
 * builder content are ignored: the caller sends no Mach reply. */
static int rb_dispatch(xw_reader *msg, xw_builder *reply, rb_req_ctx *ctx) {
    bsxpc_message parsed;
    int pr = bsxpc_message_parse(msg, &rb_env_keys, &parsed);
    if (pr != XW_OK) return SVCHOST_ERR_BAD_MESSAGE;

    /* THE "connect" REPLY RULE (see
     * fakerb.h). The incoming msgh_id is the reply-expectation
     * signal: 0x40000000 means a reply is expected, 0x10000000 means
     * the message is one-way and NO reply may be sent, even when it
     * carries a reply port. The dispatch below still runs when the
     * rule fires: its side effects (e.g. recording an assertion)
     * are not a reply, so they must happen. The caller sends no
     * Mach reply when ctx->no_reply is set, and ignores the built
     * reply and the return value. Any other msgh_id falls back to
     * the reply-port rule. The svchost adapter handler never sets
     * have_msgh_id, so the rule cannot fire there (see fakerb.h). */
    if (ctx->have_msgh_id && ctx->msgh_id == XW_MSGH_ID_XPC) {
        ctx->no_reply = 1;
    }

    if (!parsed.has_sel || parsed.sel == NULL)
        return SVCHOST_ERR_UNKNOWN_MESSAGE;
    if (rb_strcmp(parsed.sel, FAKERB_MSG_HANDSHAKE) == 0)
        return rb_handshake(&parsed, msg, reply);
    if (rb_strcmp(parsed.sel, FAKERB_MSG_HANDLE_FOR_PREDICATE) == 0)
        return rb_handle_for_predicate(&parsed, msg, reply);
    if (rb_strcmp(parsed.sel, FAKERB_MSG_HANDLE_FOR_PID) == 0)
        return rb_handle_for_pid(&parsed, msg, reply);
    if (rb_strcmp(parsed.sel, FAKERB_MSG_STATES_FOR_PREDICATE) == 0)
        return rb_states_for_predicate(&parsed, msg, reply);
    if (rb_strcmp(parsed.sel, FAKERB_MSG_ACQUIRE_ASSERTION) == 0)
        return rb_acquire_assertion(&parsed, msg, reply);
    if (rb_strcmp(parsed.sel, FAKERB_MSG_INVALIDATE_ASSERTION) == 0)
        return rb_invalidate_assertion(&parsed, msg, reply);
    if (rb_strcmp(parsed.sel, FAKERB_MSG_INVALIDATE_ASSERTION_ID) == 0)
        return rb_invalidate_assertion(&parsed, msg, reply);
    if (rb_strcmp(parsed.sel, FAKERB_MSG_SUBSCRIBE_STATE) == 0)
        return rb_subscribe_state(&parsed, msg, reply);
    if (rb_strcmp(parsed.sel, FAKERB_MSG_UNSUBSCRIBE_STATE) == 0)
        return rb_unsubscribe_state(&parsed, msg, reply);
    if (rb_strcmp(parsed.sel, FAKERB_MSG_EXECUTE_LAUNCH) == 0)
        return rb_unsupported(&parsed, msg, reply);
    if (rb_strcmp(parsed.sel, FAKERB_MSG_EXECUTE_TERMINATE) == 0)
        return rb_unsupported(&parsed, msg, reply);
    if (rb_strcmp(parsed.sel, FAKERB_MSG_SUBSCRIBE_DEATH) == 0)
        return rb_unsupported(&parsed, msg, reply);
    if (rb_strcmp(parsed.sel, FAKERB_MSG_SAVE_ENDOWMENT) == 0)
        return rb_unsupported(&parsed, msg, reply);
    return SVCHOST_ERR_UNKNOWN_MESSAGE;
}

/* svchost handler adapter: svchost_process_msg owns the reply header
 * and the never-drop-a-reply rule on this path; all this adapter
 * does is log and dispatch. */
static int rb_svchost_handler(xw_reader *msg, xw_builder *reply,
                              void *ctx) {
    rb_req_ctx rctx;
    (void)ctx;
    if (bsxpc_trace_enabled())
        (void)bsxpc_log_dict(msg, FAKERB_LOG_PREFIX);
    /* No Mach header reaches this handler, so no msgh_id is known
     * and the msgh_id rule cannot fire here: every message gets a
     * reply when svchost_process_msg sees a reply port. See
     * fakerb.h; the live-path gap is flagged in the report that
     * accompanies this file. */
    rctx.msgh_id = 0;
    rctx.have_msgh_id = 0;
    rctx.no_reply = 0;
    {
        /* Native RBS wire (iOS 26.6.2): {"rbs_selector": sel, "rbs_argument_N": obj}, reply
         * {"return-value": obj} decoded by -[RBSXPCMessage ...] 0x18f00e8ac; error under
         * "__RBSXPCError__" 0x18f00e868. Objects are dicts with "bsx_class". */
        xw_value v;
        const char *sel = 0;
        uint32_t sl = 0;
        if (xw_dict_find(msg, "rbs_selector", &v) == XW_OK &&
            xw_as_string(&v, &sel, &sl) == XW_OK && sel) {
            int r = xw_dict_begin(reply);
            if (rb_strcmp(sel, FAKERB_MSG_HANDSHAKE) == 0) {
                /* RBSHandshakeResponse initWithRBSXPCCoder 0x18f01d334: "_handle"
                 * (RBSProcessHandle), success-/fail- keys/values optional. Handle fields
                 * from -[RBSProcessHandle _fullEncode] 0x18f007cb8: _pid, _data, ... */
                if (r == XW_OK) r = xw_dict_add_dict_begin(reply, "return-value");
                if (r == XW_OK) r = xw_dict_add_string(reply, "bsx_class", "RBSHandshakeResponse");
                if (r == XW_OK) r = xw_dict_add_dict_begin(reply, "_handle");
                if (r == XW_OK) r = xw_dict_add_string(reply, "bsx_class", "RBSProcessHandle");
                if (r == XW_OK) r = xw_dict_add_dict_begin(reply, "_identity");
                /* -[RBSEmbeddedAppProcessIdentity initWithRBSXPCCoder:] 0x18f004e08 */
                if (r == XW_OK) r = xw_dict_add_string(reply, "bsx_class", "RBSEmbeddedAppProcessIdentity");
                if (r == XW_OK) r = xw_dict_add_string(reply, "_embeddedApplicationIdentifier", g_rb_self_bundle_id);
                if (r == XW_OK) r = xw_dict_end(reply);
                if (r == XW_OK) r = xw_dict_add_uint64(reply, "_pid", g_rb_self_pid);
                if (r == XW_OK) r = xw_dict_add_uint64(reply, "_data", 0);
                if (r == XW_OK) r = xw_dict_end(reply);
                if (r == XW_OK) r = xw_dict_end(reply);
            }
            else if (rb_strcmp(sel, "acquireAssertionWithDescriptor:error:") == 0) {
                /* -[RBSAssertionIdentifier initWithRBSXPCCoder:] 0x18f00e0a4: serverPid, clientPid, count */
                if (r == XW_OK) r = xw_dict_add_dict_begin(reply, "return-value");
                if (r == XW_OK) r = xw_dict_add_string(reply, "bsx_class", "RBSAssertionIdentifier");
                if (r == XW_OK) r = xw_dict_add_int64(reply, "serverPid", (int64_t)g_rb_self_pid);
                if (r == XW_OK) r = xw_dict_add_int64(reply, "clientPid", (int64_t)g_rb_self_pid);
                if (r == XW_OK) r = xw_dict_add_int64(reply, "count", (int64_t)rb_next_id());
                if (r == XW_OK) r = xw_dict_end(reply);
            }
            if (r == XW_OK) r = xw_dict_end(reply);
            if (bsxpc_trace_enabled()) {
                (void)sys_write(2, "[fakerb] native rbs reply: ", 27);
                (void)sys_write(2, sel, sl);
                (void)sys_write(2, "\n", 1);
            }
            return (r == XW_OK) ? 0 : SVCHOST_ERR_NO_MEMORY;
        }
    }
    return rb_dispatch(msg, reply, &rctx);
}

/* ------------------------------------------------------------------ */
/* Public API.                                                         */
/* ------------------------------------------------------------------ */

int fakerb_init(void) {
    if (g_rb_inited) return KERN_SUCCESS;
    {
        int kr = svchost_register(FAKERB_SERVICE_NAME, rb_svchost_handler,
                                  NULL, NULL);
        if (kr != KERN_SUCCESS) return kr;
    }
    g_rb_inited = 1;
    return KERN_SUCCESS;
}

int fakerb_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                       uint8_t *reply_buf, uint32_t max_reply_size,
                       uint32_t *out_reply_size) {
    mach_port_name_t reply_port;
    uint8_t *body;
    uint32_t body_cap;
    int one_way;
    int64_t err_code = 0;
    uint32_t body_len = 0;
    if (!hdr || send_size < sizeof(mach_msg_header_t) || !reply_buf ||
        !out_reply_size) {
        return 0;
    }
    /* The smallest reply we can ever build is the 40-byte error dict,
     * so a smaller buffer cannot satisfy the never-drop-a-reply rule. */
    if (max_reply_size < sizeof(mach_msg_header_t) + 40) return 0;
    reply_port = hdr->msgh_local_port;
    body = reply_buf + sizeof(mach_msg_header_t);
    body_cap = max_reply_size - sizeof(mach_msg_header_t);
    /* One-way message (no reply port): run the handler for its side
     * effects, send nothing. */
    one_way = (reply_port == MACH_PORT_NULL);

    {
        xw_envelope env;
        xw_reader rd;
        if (xw_envelope_parse(hdr, send_size, &env) != XW_OK ||
            xw_reader_init(&rd, env.payload, env.payload_len,
                           env.descs, env.ndescs) != XW_OK) {
            err_code = SVCHOST_ERR_BAD_MESSAGE;
        } else {
            if (bsxpc_trace_enabled())
                (void)bsxpc_log_dict(&rd, FAKERB_LOG_PREFIX);
            {
                xw_builder b;
                if (xw_builder_init(&b, body, body_cap) != XW_OK) {
                    err_code = SVCHOST_ERR_NO_MEMORY;
                } else {
                    rb_req_ctx ctx;
                    int hr;
                    /* Pass the msgh_id so rb_dispatch can enforce
                     * the "connect" reply rule (see there). */
                    ctx.msgh_id = hdr->msgh_id;
                    ctx.have_msgh_id = 1;
                    ctx.no_reply = 0;
                    hr = rb_dispatch(&rd, &b, &ctx);
                    if (ctx.no_reply) {
                        /* The msgh_id rule fired: this message is
                         * one-way, so no Mach reply is sent at all,
                         * even though a reply port exists and a
                         * reply dict may have been built. The
                         * dispatch side effects stand. */
                        *out_reply_size = 0;
                        return 1;
                    }
                    if (hr == 0) {
                        body_len = (uint32_t)xw_builder_len(&b);
                    } else {
                        err_code = hr;
                    }
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
     * this guard covers the parse-failure and builder-failure paths,
     * which reach here without it. */
    if (hdr->msgh_id == XW_MSGH_ID_XPC) {
        *out_reply_size = 0;
        return 1;
    }
    if (err_code != 0) {
        /* Handler error, malformed payload, or unknown message:
         * answer with {"error": code}. This path must not fail for a
         * reply that fits; the error dict is 40 bytes and the caller
         * was checked to have room for it above. */
        if (rb_build_error_body(body, body_cap, err_code, &body_len) != 0) {
            return 0;
        }
    }

    if (one_way) {
        *out_reply_size = 0;
        return 1;
    }

    /* v1 replies never carry port descriptors: handlers must not add
     * mach_send / mach_recv / mach_send_once values. The builder
     * records such values in b.descs, but this reply path does not
     * emit them, so a port value would be written without its
     * descriptor and the client would misparse the reply. */
    rb_fill_reply_header(reply_buf, body_len, reply_port);

    *out_reply_size = sizeof(mach_msg_header_t) + body_len;
    return 1;
}

void fakerb_set_self_pid(uint32_t pid) {
    g_rb_self_pid = pid;
}

void fakerb_set_self_bundle_id(const char *bundle_id) {
    size_t i = 0;
    if (!bundle_id || !*bundle_id) return;
    while (bundle_id[i] && i + 1 < sizeof(g_rb_self_bundle_id)) {
        g_rb_self_bundle_id[i] = bundle_id[i];
        i++;
    }
    g_rb_self_bundle_id[i] = '\0';
}

int fakerb_assertion_count(void) {
    return (int)g_rb_assertion_count;
}

int fakerb_assertion_valid(uint32_t id) {
    uint32_t i;
    for (i = 0; i < g_rb_assertion_count; i++) {
        if (g_rb_assertions[i] == id) return 1;
    }
    return 0;
}
