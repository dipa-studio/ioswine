/*
 * bplist.h: minimal binary plist (bplist00) reader/writer for ioswine.
 *
 * Freestanding, arena-backed, no libc. Used by cfprefsd (per-domain
 * plist storage), MobileGestalt (cache plist) and csops (entitlements
 * plist). Types: NULL, BOOL, INT, UINT, REAL, STRING, DATA, DATE,
 * ARRAY, DICT. The bplist00 format itself has no unsigned integer
 * type and no UID type:
 *
 * - BP_UINT is written as a bplist int of the smallest fitting size
 *   and reads back as BP_INT with the same numeric value while it
 *   fits in int64. A BP_UINT above INT64_MAX is written as 8 raw
 *   bytes and reads back as BP_INT with the same bit pattern
 *   (negative). Callers that need the exact uint64 should store it
 *   as DATA or as a decimal string instead.
 * - UID objects (marker 0x8) are rejected by the reader with
 *   XW_ERR_BAD_TYPE; the writer never emits them.
 * - Dates are bplist doubles of seconds since 2001-01-01 (Apple
 *   reference date, 978307200 Unix seconds). BP_DATE stores
 *   nanoseconds since the Unix epoch, the same convention as the
 *   xpcwire date type. A date round-trips to within about a
 *   microsecond; sub-microsecond precision is lost to the double.
 * - Strings are ASCII (marker 0x5) when every byte is below 0x80,
 *   otherwise UTF-16BE (marker 0x6). The reader converts UTF-16BE
 *   (including surrogate pairs) to UTF-8 in the arena and rejects
 *   lone surrogates. The writer rejects invalid UTF-8.
 *
 * Negative return codes are the XW_ERR_* values from xpcwire.h:
 * XW_ERR_NOSPACE (arena or output buffer exhausted), XW_ERR_TRUNCATED
 * (buffer ends inside a value), XW_ERR_BAD_TYPE (unknown marker, bad
 * size, UID, non-string dict key, invalid UTF-8/UTF-16), XW_ERR_NOT_FOUND
 * (bp_dict_get miss; also used by the reader when a dict key object
 * is missing, which cannot happen for a well-formed file),
 * XW_ERR_OVERFLOW (nesting deeper than BP_MAX_DEPTH, or more objects
 * than BP_WRITE_MAX_OBJECTS).
 *
 * No em dashes are used in this file.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "xpcwire.h"

/* Limits. */
#define BP_MAX_DEPTH 32          /* max container nesting, reader and writer */
#define BP_WRITE_MAX_OBJECTS 1024 /* max objects in one bp_write call */

typedef enum { BP_NULL=0, BP_BOOL, BP_INT, BP_UINT, BP_REAL,
               BP_STRING, BP_DATA, BP_DATE, BP_ARRAY, BP_DICT } bp_type_t;

typedef struct bp_value {
    uint32_t type;             /* bp_type_t */
    int64_t i;                 /* INT, DATE (ns since Unix epoch) */
    uint64_t u;                /* UINT */
    double d;                  /* REAL */
    int b;                     /* BOOL */
    const uint8_t *s;          /* STRING bytes (NUL-terminated in arena),
                                  DATA bytes */
    uint32_t len;              /* STRING len incl NUL, DATA len */
    struct bp_value *child;    /* ARRAY/DICT: first child */
    struct bp_value *next;     /* ARRAY/DICT: next sibling */
    const char *key;           /* DICT entry key (NULL for array items) */
} bp_value;

typedef struct { uint8_t *buf; uint32_t cap; uint32_t len; } bp_arena;

/* Initialise an arena over a caller supplied buffer. The arena never
 * grows and never frees; bp_parse and the bp_new_* constructors
 * return an error (or NULL) when it is exhausted. */
void bp_arena_init(bp_arena *a, uint8_t *buf, uint32_t cap);

/* Allocate a fresh zeroed node in the arena. NULL on exhaustion. */
bp_value *bp_new(bp_arena *a, uint32_t type);

/* Parse a bplist00 buffer into the arena. *root_out is set only on
 * success. Returns 0 or a negative XW_ERR_*. Every read is bounds
 * checked; a truncated or malformed buffer can never read past it.
 * The buffer must stay alive only for the call: all strings and
 * data are copied into the arena. */
int bp_parse(const uint8_t *buf, uint32_t len, bp_arena *a,
             bp_value **root_out);

/* Serialise a tree to bplist00. The root is written as object 0 and
 * every other object follows in pre-order, so the trailer's top
 * object is always 0. Returns 0 or a negative XW_ERR_*; *out_len is
 * set only on success. Fails with XW_ERR_OVERFLOW when the tree has
 * more than BP_WRITE_MAX_OBJECTS nodes or nests deeper than
 * BP_MAX_DEPTH. */
int bp_write(const bp_value *root, uint8_t *out, uint32_t cap,
             uint32_t *out_len);

/* Dictionary lookup by key. Returns NULL when dict is NULL, not a
 * dict, or the key is absent. First matching key wins. */
bp_value *bp_dict_get(const bp_value *dict, const char *key);

/* Set a dict entry. Replaces the value when the key already exists,
 * appends a new entry otherwise. The key is copied into the arena
 * for a new entry; for a replacement the existing key string is
 * kept. val must be a fresh node not already linked into a tree.
 * Returns 0 or a negative XW_ERR_*. */
int bp_dict_set(bp_arena *a, bp_value *dict, const char *key,
                bp_value *val);

/* Append a value to an array. val must be a fresh node not already
 * linked into a tree. Returns 0 or a negative XW_ERR_*. */
int bp_array_append(bp_arena *a, bp_value *arr, bp_value *val);

/* Constructors. NULL on arena exhaustion. bp_new_string copies the
 * string including its NUL terminator; bp_new_data copies len bytes
 * (len may be 0, then s points at the arena end and len is 0). */
bp_value *bp_new_string(bp_arena *a, const char *s);
bp_value *bp_new_int(bp_arena *a, int64_t v);
bp_value *bp_new_uint(bp_arena *a, uint64_t v);
bp_value *bp_new_bool(bp_arena *a, int b);
bp_value *bp_new_real(bp_arena *a, double v);
bp_value *bp_new_data(bp_arena *a, const void *p, uint32_t len);
