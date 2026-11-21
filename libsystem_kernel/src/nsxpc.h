/*
 * nsxpc.h: NSXPC wire protocol encoder/decoder for ioswine.
 *
 * Encapsulates the bplist17 serialization format used by Foundation's
 * NSXPCConnection in iOS 26.6.2 (build 23G90).
 *
 * Freestanding C. No libc headers beyond <stdint.h>, <stddef.h>, <stdarg.h>.
 * No em dashes are used in this file.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "xpcwire.h"
#include "mg.h"

/* ------------------------------------------------------------------ */
/* bplist17 tags and constants (iOS 26.6.2).                          */
/* ------------------------------------------------------------------ */

#define NSXPC_MAGIC              "bplist17"
#define NSXPC_MAGIC_LEN          8

#define NSXPC_TAG_INT            0x10
#define NSXPC_TAG_REAL           0x20
#define NSXPC_TAG_DATA           0x40
#define NSXPC_TAG_STRING_ASCII   0x50
#define NSXPC_TAG_STRING_UTF16   0x60
#define NSXPC_TAG_SELECTOR       0x70
#define NSXPC_TAG_UID            0x80
#define NSXPC_TAG_ARRAY          0xa0
#define NSXPC_TAG_BOOL_TRUE      0xb0
#define NSXPC_TAG_BOOL_FALSE     0xc0
#define NSXPC_TAG_DICT           0xd0
#define NSXPC_TAG_REPLY_MARKER   0xe0
#define NSXPC_TAG_NIL            0xe0

#define NSXPC_MAX_STR_LEN        128
#define NSXPC_MAX_PAYLOAD_LEN    1024

/* ------------------------------------------------------------------ */
/* Parsed NSXPC request structure.                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t proxynum;
    uint64_t sequence;
    uint64_t flags;
    const char *replysig;
    char selector[NSXPC_MAX_STR_LEN];
    char question[NSXPC_MAX_STR_LEN];
    int has_question;
} nsxpc_request_t;

/* ------------------------------------------------------------------ */
/* Public functions.                                                  */
/* ------------------------------------------------------------------ */

/* Return 1 if msg is an NSXPC message (contains "root" data or       */
/* sequence/proxynum), 0 otherwise.                                   */
int nsxpc_is_message(xw_reader *msg);

/* Parse an incoming NSXPC request message into req.                  */
/* Returns 0 on success, negative error code on malformed message.    */
int nsxpc_parse_request(xw_reader *msg, nsxpc_request_t *req);

/* Helper to resolve a selector name or signature string to the       */
/* canonical reply signature ("v@?@" or "v@?B").                      */
const char *nsxpc_reply_signature(const char *selector_or_sig);

/* Encode an invocation reply in bplist17 format into out_buf.        */
/* replysig_or_sel: reply signature ("v@?@", "v@?B") or selector      */
/* ans: answer value, or NULL for nil object                          */
/* Returns 0 on success with *out_len set, negative on error.         */
int nsxpc_encode_reply(const char *replysig_or_sel, const mg_answer_t *ans,
                       uint8_t *out_buf, uint32_t max_buf_len,
                       uint32_t *out_len);

/* Build an NSXPC reply dictionary with an mg_answer_t payload.       */
int nsxpc_build_reply_dict(xw_builder *reply, const char *replysig_or_sel,
                           const mg_answer_t *ans);

/* Build an NSXPC reply dictionary returning a BOOL argument.         */
int nsxpc_build_bool_reply_dict(xw_builder *reply, const char *replysig_or_sel,
                                int success);

/* Build an NSXPC reply dictionary returning a nil argument.          */
int nsxpc_build_nil_reply_dict(xw_builder *reply, const char *replysig_or_sel);
