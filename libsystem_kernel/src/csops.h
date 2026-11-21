/*
 * csops.h: parse an LC_CODE_SIGNATURE superblob (CS_SuperBlob) and
 * answer csops/entitlement queries for ioswine.
 *
 * The csops() syscall itself lives in bsd.c and is out of scope
 * here; this is the parser plus the answers, so the syscall can be a
 * thin wrapper over it. All multi-byte integers in the signature
 * structures are big-endian; the parser runs on little-endian arm64
 * and byte-swaps every field on read. Hashes are byte strings and
 * are never swapped.
 *
 * Every numeric constant
 * below is tagged [src: ...] or [guess] in the comment next to it;
 * the [guess] values (CS_OPS 6-10, the DER magic, the alternate and
 * launch-constraint slots, the 0x20500 tail fields) must be confirmed
 * against the XNU codesign.h / cs_blobs.h for iOS 26.6.2 before the
 * syscall wrapper hardcodes them.
 *
 * Conventions:
 * - csops_info.entitlements points at the entitlements PAYLOAD, past
 *   the 8-byte CS_GenericBlob header (magic + length). The whole blob
 *   including the header is kept in entitlements_blob, so the syscall
 *   wrapper can answer CS_OPS_ENTITLEMENTS_BLOB either way; which
 *   form the real syscall returns is still [guess].
 * - Slot 5 is preferred over slot 7 for the entitlements pointer,
 *   whatever order the index lists them in, because slot 5 is the
 *   parseable form and slot 7 DER is out of scope for this delivery.
 * - Slot numbers identify blobs; a slot 5 or slot 7 entry is recorded
 *   by slot number even if its own magic differs (the DER magic
 *   value is [guess]). The superblob magic and the CodeDirectory
 *   magic are checked strictly.
 * - A duplicate slot entry is ignored: the first occurrence wins.
 * - A superblob without a CodeDirectory still parses (return 0 with
 *   has_code_directory == 0); entitlements alone are usable.
 *
 * Arena sizing for csops_entitlements_plist and
 * csops_has_entitlement: the caller arena should hold at least the
 * entitlements payload length plus 1024 bytes. The XML reader
 * decodes text into the arena, so a payload that does not fit makes
 * the parse return NULL instead of a half-built tree.
 *
 * No em dashes are used in this file.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "xpcwire.h"      /* XW_ERR_* */
#include "bplist.h"       /* bp_value, bp_arena */
#include "csops_names.h"  /* entitlement keys, XML grammar tokens */

/* Magics [src: LLVM D96164 CodeSigningTypes.h, underlayer code
 * signing]. */
#define CS_MAGIC_SUPERBLOB          0xfade0cc0u
#define CS_MAGIC_SUPERBLOB_OLD      0xfade0b02u /* [guess] */
#define CS_MAGIC_DETACHED_SIGNATURE 0xfade0cc1u /* [guess] */
#define CS_MAGIC_REQUIREMENT        0xfade0c00u
#define CS_MAGIC_REQUIREMENTS       0xfade0c01u
#define CS_MAGIC_CODEDIRECTORY      0xfade0c02u
#define CS_MAGIC_BLOBWRAPPER        0xfade0b01u
#define CS_MAGIC_ENTITLEMENTS       0xfade7171u
#define CS_MAGIC_DER_ENTITLEMENTS   0xfade7172u /* [guess] value */

/* SuperBlob index slots (CSSLOT) [src: cctools code signing
 * integration for slots 0, 2, 5, 7, 0x10000; go-macho for slots 8,
 * 9, 10; the rest [guess]]. */
#define CS_SLOT_CODEDIRECTORY              0u
#define CS_SLOT_INFOSLOT                   1u /* [guess] */
#define CS_SLOT_REQUIREMENTS               2u
#define CS_SLOT_RESOURCEDIR                3u /* [guess] */
#define CS_SLOT_TOPSPECIFIC                4u /* [guess] */
#define CS_SLOT_ENTITLEMENTS               5u
#define CS_SLOT_REPRESENTATIVE             6u /* [guess] */
#define CS_SLOT_DER_ENTITLEMENTS           7u
#define CS_SLOT_LAUNCH_CONSTRAINT_SELF     8u
#define CS_SLOT_LAUNCH_CONSTRAINT_PROCESS  9u
#define CS_SLOT_LAUNCH_CONSTRAINT_TRUSTED  10u
#define CS_SLOT_ALTERNATE_CODEDIRECTORIES  0x1000u /* [guess] */
#define CS_SLOT_SIGNATURESLOT              0x10000u

/* CodeDirectory versions [src: folio code signing internals]. */
#define CS_CD_VERSION_20100 0x20100u
#define CS_CD_VERSION_20200 0x20200u
#define CS_CD_VERSION_20300 0x20300u
#define CS_CD_VERSION_20400 0x20400u
#define CS_CD_VERSION_20500 0x20500u

/* CodeDirectory hash types [src: underlayer code signing].
 * hashSize follows the type: 20, 32, 32, 48 [guess: derived from
 * the algorithm, not directly sourced]. */
#define CS_HASH_TYPE_SHA1             1u
#define CS_HASH_TYPE_SHA256           2u
#define CS_HASH_TYPE_SHA256_TRUNCATED 3u
#define CS_HASH_TYPE_SHA384           4u

/* Code signing status flags [src: folio code signing internals]. */
#define CS_VALID         0x00000001u
#define CS_ADHOC         0x00000002u
#define CS_GET_TASK_ALLOW 0x00000004u
#define CS_HARD          0x00000100u
#define CS_KILL          0x00000200u
#define CS_REQUIRE_LV    0x00002000u
#define CS_RUNTIME       0x00010000u
#define CS_LINKER_SIGNED 0x00020000u

/* csops() operation codes [src: xnu-1228.3.13 bsd/sys/codesign.h via
 * the Valgrind Darwin csops patch for 0-5; xnu-7195.50.7.100.1 for
 * 15. Values 6-10 are [guess]: no returned source printed them, and
 * they must be confirmed against the XNU codesign.h for iOS 26.6.2
 * before bsd.c hardcodes them. A wrong op number makes the syscall
 * return EINVAL or, worse, run a setter. */
#define CS_OPS_STATUS           0u
#define CS_OPS_MARKINVALID      1u
#define CS_OPS_MARKHARD         2u
#define CS_OPS_MARKKILL         3u
#define CS_OPS_PIDPATH          4u
#define CS_OPS_CDHASH           5u
#define CS_OPS_PIDOFFSET        6u /* [guess] */
#define CS_OPS_ENTITLEMENTS_BLOB 7u /* [guess] */
#define CS_OPS_IDENTITY         8u /* [guess] */
#define CS_OPS_BLOB             9u /* [guess] */
#define CS_OPS_TEAMID           10u /* [guess] */
#define CS_OPS_CLEAR_LV         15u

/* SHA-256 digest length in bytes. */
#define CS_SHA256_LEN 32u

typedef struct {
    const uint8_t *entitlements;   /* entitlements blob payload bytes
                                      (plist or DER), past the 8-byte
                                      CS_GenericBlob header */
    uint32_t entitlements_len;
    int entitlements_is_der;       /* 1 when DER, 0 when plist */
    int has_code_directory;
    int has_entitlements;
    /* Additive fields:
     * the whole entitlements blob including its header, and the
     * CodeDirectory blob, so the syscall wrapper can answer
     * CS_OPS_ENTITLEMENTS_BLOB with or without the header and can
     * compute the cdhash without re-parsing. All pointers borrow
     * from the superblob passed to csops_parse; they stay valid
     * only while that buffer lives. */
    const uint8_t *entitlements_blob; /* whole slot 5/7 blob incl. header */
    uint32_t entitlements_blob_len;
    const uint8_t *code_directory;    /* slot 0 CodeDirectory bytes */
    uint32_t code_directory_len;      /* CD internal length field */
} csops_info;

/* Parse a SuperBlob (the LC_CODE_SIGNATURE data). Validates the
 * magic, the length and count against the supplied buffer, every
 * index offset and blob length against the SuperBlob length, and
 * the CodeDirectory header when slot 0 is present. Returns 0 or a
 * negative XW_ERR_*: XW_ERR_BAD_MAGIC for a wrong superblob or
 * CodeDirectory magic, XW_ERR_BAD_VERSION for an unknown
 * CodeDirectory version, XW_ERR_TRUNCATED for any short or
 * out-of-bounds field, XW_ERR_BAD_TYPE for an inconsistent
 * CodeDirectory (unknown hash type, hash array or identifier
 * outside the blob, nCodeSlots inconsistent with codeLimit and
 * pageSize). Never reads past the supplied buffer. On success *out
 * is filled in; on failure *out is left zeroed. */
int csops_parse(const uint8_t *superblob, uint32_t len,
                csops_info *out);

/* Parse the entitlements payload into bp_value nodes in the caller
 * arena. Slot 5 is an XML plist in the adopted reading; a slot 5
 * binary plist is accepted via bplist.h when one is present (the
 * two sources conflict on the encoding, csops.md section 6.1).
 * Returns the root dict, or NULL when there are no entitlements,
 * when the blob is DER-only (slot 7 DER is out of scope for this
 * delivery: no DER reader exists yet), when the payload is neither
 * XML nor bplist00, when the root is not a dict, or when the arena
 * runs out. The arena is scratch: a failed parse may leave it
 * partly used. */
bp_value *csops_entitlements_plist(const csops_info *info,
                                   bp_arena *a);

/* 1 when the entitlements contain key, 0 when they do not, negative
 * (XW_ERR_*) on error. A key present with a boolean false value
 * counts as 0: holding an entitlement means holding it enabled,
 * which is what get-task-allow false means in practice. A
 * malformed entitlements payload gives a negative return, and a
 * DER-only blob gives a negative return (its contents cannot be
 * determined without a DER reader), never a silent 0. No
 * entitlements at all is 0, not an error. */
int csops_has_entitlement(const csops_info *info, const char *key,
                          bp_arena *a);

/* Additive answers the csops() syscall wrapper in bsd.c needs;
 * they only read the parsed info and never change it. */

/* SHA-256 (FIPS 180-4), written fresh for this file, freestanding.
 * The tree has no SHA-256; this is the one the cdhash needs. out
 * must hold CS_SHA256_LEN bytes. */
void cs_sha256(const uint8_t *data, uint32_t len,
               uint8_t out[CS_SHA256_LEN]);

/* The cdhash: the hash of the CodeDirectory blob itself, with the
 * CD's hash algorithm. SHA-256 CDs return CS_SHA256_LEN bytes;
 * SHA-256-truncated CDs return the first hashSize bytes of the
 * SHA-256 (hashSize from the CD header, [guess] semantics).
 * Returns 0 or a negative XW_ERR_*: XW_ERR_NOT_FOUND when there is
 * no CodeDirectory, XW_ERR_NOSPACE when out_cap is too small,
 * XW_ERR_BAD_TYPE for a SHA-1 or SHA-384 CD (no SHA-1 or SHA-384
 * implementation exists in the tree, so a fabricated hash is never
 * returned). *out_len is set only on success. */
int csops_cdhash(const csops_info *info, uint8_t *out, uint32_t out_cap,
                 uint32_t *out_len);

/* The CS_OPS_STATUS answer for a parsed signature. The default is
 * CS_VALID | CS_ADHOC, with no CS_REQUIRE_LV, no CS_HARD, no
 * CS_KILL and no CS_RUNTIME (design decision:
 * ioswine loads Apple binaries without a real signature check, so
 * the honest answer is synthetic). CS_GET_TASK_ALLOW is added when
 * the entitlements contain get-task-allow true. Returns 0 when
 * info is NULL or has no CodeDirectory (a process with no valid
 * signature has p_csflags == 0). When a is NULL, or the
 * entitlements are DER-only or unparseable, the base flags are
 * returned without the get-task-allow check. The arena is scratch
 * for the entitlement parse. */
uint32_t csops_status_flags(const csops_info *info, bp_arena *a);

/* The team id string from the CodeDirectory teamOffset field
 * (CS_OPS_TEAMID, [guess] semantics), NUL-terminated into out.
 * Returns 0 or a negative XW_ERR_*: XW_ERR_NOT_FOUND when there is
 * no CodeDirectory, no teamOffset field (version before 0x20200)
 * or an empty team string, XW_ERR_NOSPACE when cap is too small. */
int csops_team_id(const csops_info *info, char *out, uint32_t cap);

/* The CodeDirectory identifier string from identOffset
 * (CS_OPS_IDENTITY, [guess] semantics), NUL-terminated into out.
 * Returns 0 or a negative XW_ERR_*: XW_ERR_NOT_FOUND when there is
 * no CodeDirectory, XW_ERR_NOSPACE when cap is too small. */
int csops_identifier(const csops_info *info, char *out, uint32_t cap);
