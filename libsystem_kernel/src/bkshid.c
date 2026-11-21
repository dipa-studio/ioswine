/*
 * bkshid.c: in-process fake for the BackBoardServices HID event
 * delivery service.
 *
 * Endpoint "com.apple.backboard.hid-services.xpc" [guess, strong].
 * Transport is BSXPC over svchost; no new kobject, no ipc.c branch.
 *
 * Reply rules:
 *  - Every parseable message gets a benign success reply: the
 *    request CID echoed with CIDr=true. The connect handshake also
 *    carries "error": 0, mirroring fakeboard's connect reply.
 *  - An unknown selector gets the same benign success shape with no
 *    "error" key, never a bare {"error": ...} dict.
 *  - A message bsxpc_message_parse rejects has no CID to echo, so
 *    the handler returns SVCHOST_ERR_BAD_MESSAGE and svchost answers
 *    {"error": 22}; that path is for unparseable input only.
 *  - One-way messages are handled for side effects and get no Mach
 *    reply; svchost_process_msg already treats them that way, and
 *    this handler never sees the Mach header, so it just builds the
 *    reply and returns 0 like any other message.
 *
 * Selector spellings for record save / transaction begin / end are
 * [open]: no spelling is asserted as fact. The
 * handler classifies selectors with substring heuristics for its
 * diagnostic flags only (see bkshid_classify), logs every selector
 * with its CID and its bsxpc_context keys, and replies success to
 * all of them. The first device run fills in the real list from
 * the [bkshid] log; the flags never change the reply.
 *
 * Style: freestanding C11, no libc, no malloc, no static
 * function-pointer tables (dispatch is an if-chain).
 */
#include "bkshid.h"
#include "bsxpc.h"
#include "ipc.h"
#include "linux.h"

extern int g_ipc_trace;

/* ------------------------------------------------------------------ */
/* Freestanding helpers                                               */
/* ------------------------------------------------------------------ */

static size_t bh_strlen(const char *s) {
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static int bh_strcmp(const char *a, const char *b) {
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return *(const unsigned char *)a - *(const unsigned char *)b;
}

/* Substring search. Returns 1 when needle occurs in haystack. */
static int bh_strstr(const char *haystack, const char *needle) {
    size_t hn, nn, i, j;
    if (!haystack || !needle) return 0;
    hn = bh_strlen(haystack);
    nn = bh_strlen(needle);
    if (nn == 0) return 1;
    if (nn > hn) return 0;
    for (i = 0; i + nn <= hn; i++) {
        for (j = 0; j < nn; j++) {
            if (haystack[i + j] != needle[j]) break;
        }
        if (j == nn) return 1;
    }
    return 0;
}

static void bh_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static void bh_memset(void *dst, int v, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)v;
}

static void bh_print_str(const char *s) {
    if (s) sys_write(2, s, bh_strlen(s));
}

static void bh_print_hex(uint64_t val) {
    char buf[24];
    int i = 23;
    buf[i--] = '\0';
    if (val == 0) {
        buf[i--] = '0';
    } else {
        const char hex[] = "0123456789abcdef";
        while (val > 0 && i >= 0) {
            buf[i--] = hex[val & 0xf];
            val >>= 4;
        }
    }
    bh_print_str(&buf[i + 1]);
}

/* ------------------------------------------------------------------ */
/* Envelope keys.          */
/* ------------------------------------------------------------------ */

static const bsxpc_envelope_keys BKSHID_ENV_KEYS = {
    .key_bsxpc = "bsxpc",
    .key_context = "bsxpc_context",
    .key_cid = "bsxpc_CID",
    .key_cidr = "bsxpc_CIDr",
    .key_sel = "bsxpc_SEL"
};

/* ------------------------------------------------------------------ */
/* Server-side state. Single manager, single connection              */
/* in practice; a second connect just re-marks the connection        */
/* established and counts again.                                     */
/* ------------------------------------------------------------------ */

static int g_bkshid_inited = 0;
static mach_port_name_t g_bkshid_port = MACH_PORT_NULL;
static int g_connected = 0;
static int g_txn_open = 0;
static uint32_t g_connects = 0;
static uint32_t g_messages = 0;
static uint32_t g_record_saves = 0;
static char g_last_sel[128];

int bkshid_is_enabled(void) {
    const char *env = lsk_getenv("IOSWINE_BKSHID");
    if (env && env[0] == '0' && env[1] == '\0') return 0;
    return 1;
}

void bkshid_get_state(bkshid_state_t *out) {
    if (!out) return;
    bh_memset(out, 0, sizeof(*out));
    out->connected = g_connected;
    out->txn_open = g_txn_open;
    out->connects = g_connects;
    out->messages = g_messages;
    out->record_saves = g_record_saves;
    bh_memcpy(out->last_sel, g_last_sel, sizeof(out->last_sel));
    out->last_sel[sizeof(out->last_sel) - 1] = '\0';
}

#ifdef BKSHID_HOST_TEST
void bkshid_test_reset(void) {
    g_connected = 0;
    g_txn_open = 0;
    g_connects = 0;
    g_messages = 0;
    g_record_saves = 0;
    bh_memset(g_last_sel, 0, sizeof(g_last_sel));
}
#endif

/* ------------------------------------------------------------------ */
/* Selector classification ([guess] heuristics, diagnostics only).   */
/* The flags never change the reply: every selector gets the same    */
/* benign success shape. A wrong guess here mislabels a log line,   */
/* it cannot re-create the failure.                                  */
/* ------------------------------------------------------------------ */

#define BKSHID_CLS_OTHER      0
#define BKSHID_CLS_TXN_BEGIN  1
#define BKSHID_CLS_TXN_END    2
#define BKSHID_CLS_RECORD     3
#define BKSHID_CLS_REGISTER   4

static int bkshid_classify(const char *sel) {
    if (!sel) return BKSHID_CLS_OTHER;
    if (bh_strstr(sel, "begin") || bh_strstr(sel, "Begin") ||
        bh_strstr(sel, "startTransaction") ||
        bh_strstr(sel, "openTransaction")) {
        return BKSHID_CLS_TXN_BEGIN;
    }
    if (bh_strstr(sel, "endTransaction") ||
        bh_strstr(sel, "EndTransaction") ||
        bh_strstr(sel, "commit") || bh_strstr(sel, "Commit") ||
        bh_strstr(sel, "closeTransaction") ||
        bh_strstr(sel, "finishTransaction")) {
        return BKSHID_CLS_TXN_END;
    }
    if (bh_strstr(sel, "ecord") || bh_strstr(sel, "efer")) {
        return BKSHID_CLS_RECORD;
    }
    if (bh_strstr(sel, "egister") || bh_strstr(sel, "nitialize") ||
        bh_strstr(sel, "andshake")) {
        return BKSHID_CLS_REGISTER;
    }
    return BKSHID_CLS_OTHER;
}

static const char *bkshid_cls_tag(int cls) {
    switch (cls) {
    case BKSHID_CLS_TXN_BEGIN:
        return "txn-begin";
    case BKSHID_CLS_TXN_END:
        return "txn-end";
    case BKSHID_CLS_RECORD:
        return "record";
    case BKSHID_CLS_REGISTER:
        return "register";
    default:
        return "other";
    }
}

/* ------------------------------------------------------------------ */
/* Logging. Gated on g_ipc_trace (the first device run adds          */
/* IOSWINE_IPC_TRACE=1). One line per message: tag, selector, CID,   */
/* transaction flag, record-save count, bsxpc_context keys, and the  */
/* instance name when the context carries it (FrontBoard puts        */
/* the service name at bsxpc_context.message.s).                    */
/* ------------------------------------------------------------------ */

static void bh_log_append(char *buf, size_t *pos, size_t cap,
                          const char *s) {
    while (s && *s && *pos + 1 < cap) buf[(*pos)++] = *s++;
}

static void bh_log_append_uint(char *buf, size_t *pos, size_t cap,
                               uint64_t val) {
    char tmp[24];
    size_t n = 0;
    if (val == 0) {
        tmp[n++] = '0';
    } else {
        char rev[24];
        size_t rn = 0;
        while (val > 0 && rn < sizeof(rev)) {
            rev[rn++] = (char)('0' + (val % 10));
            val /= 10;
        }
        while (rn > 0) tmp[n++] = rev[--rn];
    }
    for (size_t i = 0; i < n && *pos + 1 < cap; i++) buf[(*pos)++] = tmp[i];
}

static void bkshid_log_msg(const bsxpc_message *m, const char *tag) {
    char line[768];
    size_t pos = 0;
    const char *sel = (m && m->has_sel) ? m->sel : "-";

    bh_log_append(line, &pos, sizeof(line), "[bkshid] ");
    bh_log_append(line, &pos, sizeof(line), tag);
    bh_log_append(line, &pos, sizeof(line), " sel=");
    bh_log_append(line, &pos, sizeof(line), sel);
    bh_log_append(line, &pos, sizeof(line), " cid=");
    bh_log_append_uint(line, &pos, sizeof(line), m ? m->cid : 0);
    bh_log_append(line, &pos, sizeof(line), " txn=");
    bh_log_append_uint(line, &pos, sizeof(line), (uint64_t)g_txn_open);
    bh_log_append(line, &pos, sizeof(line), " saves=");
    bh_log_append_uint(line, &pos, sizeof(line), (uint64_t)g_record_saves);

    if (m && m->has_ctx) {
        xw_dict_iter it;
        const char *key = NULL;
        xw_value v;
        int nkeys = 0;
        bh_log_append(line, &pos, sizeof(line), " ctxkeys=");
        if (xw_dict_iter_init_v(&m->ctx, &it) == XW_OK) {
            int r;
            while ((r = xw_dict_next(&it, &key, &v)) == 1 && nkeys < 8) {
                if (nkeys > 0) bh_log_append(line, &pos, sizeof(line), ",");
                if (key) {
                    size_t k = 0;
                    while (key[k] && k < 32 && pos + 1 < sizeof(line)) {
                        line[pos++] = key[k++];
                    }
                }
                nkeys++;
            }
        }
        /* Instance name, FrontBoard shape: bsxpc_context.message.s
         * [open: confirm from the first device trace]. */
        {
            xw_value mv, sv;
            const char *inst = NULL;
            uint32_t ilen = 0;
            if (xw_dict_find_v(&m->ctx, "message", &mv) == XW_OK &&
                xw_dict_find_v(&mv, "s", &sv) == XW_OK &&
                xw_as_string(&sv, &inst, &ilen) == XW_OK && inst) {
                bh_log_append(line, &pos, sizeof(line), " instance=");
                bh_log_append(line, &pos, sizeof(line), inst);
            }
        }
    }
    if (pos + 1 < sizeof(line)) line[pos++] = '\n';
    line[pos] = '\0';
    sys_write(2, line, pos);
}

/* ------------------------------------------------------------------ */
/* Reply opening. When the request carried a CID, open a completion  */
/* reply (same CID, CIDr=true); otherwise open a plain dict. The     */
/* caller adds the result fields and closes with xw_dict_end.       */
/* (Same shape as fakeboard's fb_reply_begin.)                       */
/* ------------------------------------------------------------------ */

static int bkshid_reply_begin(xw_builder *b, const bsxpc_message *m) {
    if (m != NULL && m->has_cid) {
        return bsxpc_reply_build(b, &BKSHID_ENV_KEYS, m->cid);
    }
    return xw_dict_begin(b);
}

/* ------------------------------------------------------------------ */
/* Handler                                                              */
/* ------------------------------------------------------------------ */

int bkshid_svchost_handler(xw_reader *msg, xw_builder *reply, void *ctx) {
    bsxpc_message parsed;
    int cls = BKSHID_CLS_OTHER;
    const char *tag = "other";
    int is_connect = 0;
    int is_teardown = 0;
    int r;

    (void)ctx;

    if (!msg || !reply) return SVCHOST_ERR_BAD_MESSAGE;

    bh_memset(&parsed, 0, sizeof(parsed));
    if (bsxpc_message_parse(msg, &BKSHID_ENV_KEYS, &parsed) != XW_OK) {
        return SVCHOST_ERR_BAD_MESSAGE;
    }
    g_messages++;

    if (parsed.has_kind && parsed.kind) {
        if (bh_strcmp(parsed.kind, "connect") == 0) {
            is_connect = 1;
            tag = "connect";
        } else if (bh_strcmp(parsed.kind, "invalidate") == 0 ||
                   bh_strcmp(parsed.kind, "interrupt") == 0) {
            is_teardown = 1;
            tag = "teardown";
        }
    }

    if (parsed.has_sel && parsed.sel) {
        size_t n = bh_strlen(parsed.sel);
        if (n >= sizeof(g_last_sel)) n = sizeof(g_last_sel) - 1;
        bh_memcpy(g_last_sel, parsed.sel, n);
        g_last_sel[n] = '\0';
        if (!is_connect && !is_teardown) {
            cls = bkshid_classify(parsed.sel);
            tag = bkshid_cls_tag(cls);
        }
    }

    if (is_connect) {
        g_connected = 1;
        g_connects++;
    } else if (is_teardown) {
        g_connected = 0;
        g_txn_open = 0;
    } else if (cls == BKSHID_CLS_TXN_BEGIN) {
        g_txn_open = 1;
    } else if (cls == BKSHID_CLS_TXN_END) {
        g_txn_open = 0;
    } else if (cls == BKSHID_CLS_RECORD) {
        /* Count the save and reply success regardless of the flag:
         * the client's assertion is about its transaction state,
         * which it manages once the manager is live; refusing here
         * would re-create the failure. */
        g_record_saves++;
    }

    if (g_ipc_trace) bkshid_log_msg(&parsed, tag);

    /* Benign success reply for every parseable message. Connect
     * mirrors fakeboard's connect reply (CID, CIDr, "error": 0);
     * every other selector gets CID + CIDr only, with no "error"
     * key (fallback). */
    r = bkshid_reply_begin(reply, &parsed);
    if (r == XW_OK && is_connect) {
        r = xw_dict_add_int64(reply, "error", 0);
    }
    if (r == XW_OK) r = xw_dict_end(reply);
    return (r == XW_OK) ? 0 : SVCHOST_ERR_NO_MEMORY;
}

/* ------------------------------------------------------------------ */
/* Registration                                                         */
/* ------------------------------------------------------------------ */

int bkshid_init(void) {
    mach_port_name_t port = MACH_PORT_NULL;
    int kr;

    if (g_bkshid_inited) return KERN_SUCCESS;
    if (!bkshid_is_enabled()) return KERN_SUCCESS;

    kr = svchost_register(BKSHID_SERVICE_NAME, bkshid_svchost_handler,
                          NULL, &port);
    if (kr != KERN_SUCCESS) return kr;
    g_bkshid_port = port;
    g_bkshid_inited = 1;

    if (g_ipc_trace) {
        bh_print_str("[bkshid] service ");
        bh_print_str(BKSHID_SERVICE_NAME);
        bh_print_str(" registered port=0x");
        bh_print_hex((uint64_t)port);
        bh_print_str("\n");
    }
    return KERN_SUCCESS;
}
