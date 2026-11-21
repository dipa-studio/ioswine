/*
 * mg.h: MobileGestalt service for ioswine.
 *
 * Two mechanisms answer a MobileGestalt question, and this service
 * owns both:
 *
 * 1. The on-disk cache file, the primary mechanism. libMobileGestalt
 *    reads it directly; most launch-time questions never reach the
 *    helper. mg_init() writes the file when it is missing or stale
 *    (CacheVersion must be 23G90). The file is a bplist00 document
 *    built with the bplist module from the static answer table in
 *    mg.c. Paths come from container.h, never hardcoded.
 * 2. The MobileGestaltHelper XPC service (com.apple.mobilegestalt.xpc)
 *    via svchost. It answers questions the cache cannot hold:
 *    protected identifiers (denied, never fabricated) and dynamic
 *    values (NULL answer). The request shape {"question": "..."} is
 *    the only shape buildable without a device trace and is probably
 *    wrong for the real NSXPC helper [guess]; the handler also scans
 *    any top-level string value for a known question, and logs every
 *    incoming message verbatim with bsxpc_log_dict when
 *    IOSWINE_SVC_TRACE=1, so the first device trace can correct it.
 *
 * Freestanding C. No function-pointer tables in static data.
 *
 * lsk file I/O: the tree has no bsd.h; lsk_open/lsk_read/lsk_write/
 * lsk_close/lsk_mkdir are defined in the tree's bsd.c (Darwin flags
 * translated to Linux by darwin_open_flags_to_linux) and are declared
 * here so mg.c compiles freestanding against that tree. The host
 * test provides libc-backed stubs with the same signatures.
 *
 * mg.c includes container.h and calls only container_group().
 * container_group() does NOT build a path from the id: groups are
 * pre-registered by the launcher with container_set_group(), and an
 * unregistered id returns XW_ERR_NOT_FOUND. LAUNCHER REQUIREMENT: before daemons_init(),
 * the launcher must register
 *   container_set_group("systemgroup.com.apple.mobilegestaltcache",
 *       "<system-group root>")
 * where the root is the directory whose
 * Library/Caches/com.apple.MobileGestalt.plist is the documented
 * cache path (on a real device:
 * /private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache).
 * If the group is not registered, mg_ensure_cache() silently does
 * nothing (mg_init still succeeds and the XPC service still
 * answers); the cache file is the primary mechanism, so the
 * registration must not be skipped. mg does not register the group
 * itself: the launcher owns the fake-filesystem layout and mg must
 * not guess where the group root is mapped.
 *
 * No em dashes are used in this file.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "ipc.h"
#include "xpcwire.h"
#include "svchost.h"

/* ------------------------------------------------------------------ */
/* File I/O (defined in the tree's bsd.c; see the file header).        */
/* ------------------------------------------------------------------ */

int lsk_open(const char *path, int flags, ...) __asm__("_open");
int64_t lsk_read(int fd, void *buf, size_t count) __asm__("_read");
int64_t lsk_write(int fd, const void *buf, size_t count) __asm__("_write");
int lsk_close(int fd) __asm__("_close");
int lsk_mkdir(const char *path, int mode) __asm__("_mkdir");

/* Darwin open flags used here [src: tree bsd.c,
 * darwin_open_flags_to_linux]. */
#define MG_O_RDONLY 0
#define MG_O_WRONLY 1
#define MG_O_CREAT  0x0200
#define MG_O_TRUNC  0x0400

/* ------------------------------------------------------------------ */
/* Cache constants.                                                    */
/* ------------------------------------------------------------------ */

/* CacheData blob length. The blob is opaque and zero-filled.
 * DeepWiki reports the iPhone13,4 blob at about 130 base64 lines
 * (lines 8-131 of the XML plist), roughly 6-7 KB depending on line
 * width; 7168 bytes (7 KiB) is used [guess]. */
#define MG_CACHEDATA_LEN 7168

/* Static buffers. The built cache is CacheData plus the CacheExtra
 * tree (about 3 KB), so 16 KiB covers the file with room; the read
 * buffer must hold the file we wrote. The arena must hold the whole
 * tree plus a copy of the 7168-byte CacheData blob: roughly 9 KiB of
 * nodes and strings plus 7168 bytes of data, about 17 KiB, so the
 * arena is 32 KiB for about 2x headroom. */
#define MG_CACHE_BUILD_CAP 16384
#define MG_CACHE_READ_CAP 16384
#define MG_CACHE_ARENA_CAP 32768
#define MG_CACHE_PATH_CAP 512

/* ------------------------------------------------------------------ */
/* Answer values.                                                      */
/* ------------------------------------------------------------------ */

typedef enum {
    MG_ANS_STRING = 0,
    MG_ANS_INT,
    MG_ANS_BOOL,
    MG_ANS_REAL,
    MG_ANS_DATA,
    MG_ANS_STRING_ARRAY,
    MG_ANS_INT_ARRAY,
    MG_ANS_ARTWORK_DICT  /* nested ArtworkTraits dict; no scalar fields */
} mg_answer_kind_t;

typedef struct {
    int kind;                    /* mg_answer_kind_t */
    const char *s;               /* STRING */
    int64_t i;                   /* INT */
    double d;                    /* REAL */
    int b;                       /* BOOL */
    const uint8_t *data;         /* DATA bytes */
    uint32_t data_len;           /* DATA length */
    const char * const *strs;    /* STRING_ARRAY elements */
    const int64_t *ints;         /* INT_ARRAY elements */
    uint32_t count;              /* ARRAY length */
} mg_answer_t;

/* Look up a launch-time question by its plaintext name. Returns 0
 * and fills *out (which may be NULL for a presence check) when the
 * question is in the static table, or a negative XW_ERR_NOT_FOUND
 * when it is not. Protected and dynamic questions are NOT in the
 * table; use mg_is_protected() / mg_is_dynamic() for those. */
int mg_answer_for(const char *question, mg_answer_t *out);

/* 1 when the question is entitlement-gated and must be denied
 * (UniqueDeviceID, SerialNumber, ...), 0 otherwise. */
int mg_is_protected(const char *question);

/* 1 when the question has no static answer and the helper must
 * answer NULL (BatteryCurrentCapacity, AirplaneMode, ...), 0
 * otherwise. */
int mg_is_dynamic(const char *question);

/* ------------------------------------------------------------------ */
/* Cache file.                                                         */
/* ------------------------------------------------------------------ */

/* Full cache file path into out (container_group() + relative
 * path). Returns 0 or a negative XW_ERR_*. */
int mg_cache_path(char *out, size_t cap);

/* Build the cache document (bplist00) into out. Returns 0 or a
 * negative XW_ERR_*; *out_len is set only on success. */
int mg_build_cache(uint8_t *out, uint32_t cap, uint32_t *out_len);

/* Write the cache file (creating Library/Caches under the group
 * container when needed). Returns 0 or a negative XW_ERR_*. */
int mg_write_cache(void);

/* 1 when the cache file exists, parses, has all four top-level
 * keys, carries CacheVersion 23G90 and the ProductType answer,
 * 0 otherwise. This is the __MGCacheValid() equivalent. */
int mg_cache_valid(void);

/* Write the cache when it is missing or stale. Best effort: returns
 * 0 when the cache is valid afterwards, negative otherwise. */
int mg_ensure_cache(void);

/* ------------------------------------------------------------------ */
/* Service.                                      */
/* ------------------------------------------------------------------ */

/* Register com.apple.mobilegestalt.xpc with svchost (idempotent)
 * and best-effort write the cache file first. Returns KERN_SUCCESS
 * or the registration error; a cache-write failure does not fail
 * init, the XPC service still answers. */
int mg_init(void);

/* Kobject entry point, same shape as svchost_process_msg. The mg
 * service is a svchost service, so this delegates to
 * svchost_process_msg, which dispatches to mg_svchost_handler by
 * port; delegating keeps one dispatch core instead of a copy. */
int mg_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                   uint8_t *reply_buf, uint32_t max_reply_size,
                   uint32_t *out_reply_size);

/* svchost handler: answer one question. Return 0 means the reply
 * was built in the builder; any other value goes verbatim into the
 * reply dict's "error" key. Replies:
 *   known question:   {"answer": <value>, "error": 0}
 *   protected:        {"answer": null, "error": 1} (denied)
 *   dynamic:          {"answer": null, "error": 0} (NULL answer)
 *   unknown question: {"error": 1} (handler returns
 *                     SVCHOST_ERR_UNKNOWN_MESSAGE)
 *   malformed:        {"error": 22} (handler returns
 *                     SVCHOST_ERR_BAD_MESSAGE)
 * A reply that would exceed SVCHOST_MAX_REPLY_BODY returns
 * SVCHOST_ERR_NO_MEMORY; no reply is ever silently truncated. */
int mg_svchost_handler(xw_reader *msg, xw_builder *reply, void *ctx);
