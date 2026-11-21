/*
 * csops_names.h: wire-string table for csops.
 *
 * Same pattern as fakeboard_names.h and daemon_names.h: every string
 * that is a wire value in the code signature / entitlements domain
 * lives here and nowhere else, so a wrong string can be corrected
 * after the first device trace without touching any logic. In
 * particular csops.c carries no string literals of its own: the XML
 * plist grammar tokens, the entitlement keys and the bplist magic
 * are all defined here.
 *
 * XML element and entity names follow the Apple plist grammar.
 *
 * No em dashes are used in this file.
 */
#pragma once

/* Entitlement keys. */
#define CSOPS_ENT_GET_TASK_ALLOW \
    "get-task-allow"
#define CSOPS_ENT_APPLICATION_IDENTIFIER \
    "application-identifier"
#define CSOPS_ENT_APPLICATION_GROUPS \
    "com.apple.security.application-groups"
#define CSOPS_ENT_APS_ENVIRONMENT \
    "aps-environment"
#define CSOPS_ENT_ASSOCIATED_DOMAINS \
    "com.apple.developer.associated-domains"
#define CSOPS_ENT_CLEAR_LIBRARY_VALIDATION \
    "com.apple.private.security.clear-library-validation"

/* Binary plist magic, for the slot 5 payload sniff in
 * csops_entitlements_plist. */
#define CSOPS_BPLIST_MAGIC "bplist00"

/* XML plist element names. */
#define CSOPS_XML_PLIST   "plist"
#define CSOPS_XML_DICT    "dict"
#define CSOPS_XML_KEY     "key"
#define CSOPS_XML_STRING  "string"
#define CSOPS_XML_INTEGER "integer"
#define CSOPS_XML_REAL    "real"
#define CSOPS_XML_TRUE    "true"
#define CSOPS_XML_FALSE   "false"
#define CSOPS_XML_ARRAY   "array"
#define CSOPS_XML_DATA    "data"
#define CSOPS_XML_DATE    "date"

/* XML entity names (without the leading ampersand and the trailing
 * semicolon). */
#define CSOPS_XML_ENT_AMP  "amp"
#define CSOPS_XML_ENT_LT   "lt"
#define CSOPS_XML_ENT_GT   "gt"
#define CSOPS_XML_ENT_QUOT "quot"
#define CSOPS_XML_ENT_APOS "apos"

/* XML comment opener. Any other "<!" sequence starts a declaration
 * (for example DOCTYPE) and is skipped to its closing angle bracket. */
#define CSOPS_XML_COMMENT_OPEN "<!--"
