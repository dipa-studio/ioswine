/* tuikb_validate.h - bisect the TUIKB deserializationCache corruption.
 *
 * The deterministic repro (tap the Library "Find in Library" search
 * field at 320,560) crashes every time in
 * -[TUIKBGraphSerialization objectForOffset:]+72, in the
 * [[self deserializationCache] objectForKey:@(offset)] lookup, with
 * the dictionary bucket holding -1 / garbage. The dictionary's raw
 * storage changes underneath live code on the main thread, and no
 * writer has been found yet.
 *
 * This module swizzles -[TUIKBGraphSerialization objectForOffset:]
 * to a wrapper that validates the deserializationCache dictionary
 * after every call (and before every call) and logs the first
 * corrupt step. It keeps a side table of the last 16 decoded
 * (offset, object) pairs; before each call it re-looks-up every
 * recorded offset in the cache and compares the returned pointer,
 * and after each call it records the new pair and validates again.
 * The first mismatch is logged with the offset, the expected
 * object and the object the cache now returns, and the process
 * traps there, so the log plus the crash point at the exact decode
 * step where the storage first went bad. The writer ran between the
 * last good validation and the first bad one.
 *
 * This is a DEBUG tool, not a fix. It is OFF by default and enabled
 * only with IOSWINE_TUIKB_VALIDATE=1. It costs one objectForKey:
 * lookup per recorded offset per objectForOffset: call (16 at
 * most), which is negligible against a keyboard-layout decode.
 * It does not lock anything and does not change the decode; it
 * only observes. It composes with tuikb_lock (which
 * serializes the decode); install both and the validation runs
 * inside the lock.
 *
 * Freestanding C11, no libc, no malloc, no __thread. Follows the
 * CADisplayLink swizzle pattern in cadisplay.c (dlsym via the dyld
 * gAPIs, PAC-sign the replacement IMP, all-or-nothing install).
 * No function-pointer tables in static data.
 *
 * No em dashes are used in this file.
 */
#ifndef TUIKB_VALIDATE_H
#define TUIKB_VALIDATE_H

#include <stdint.h>
#include <stddef.h>

/* Install the objectForOffset: validation wrapper. Returns 0 when
 * the hooks are installed (or were already installed), -1 when they
 * are disabled (IOSWINE_TUIKB_VALIDATE is not "1", or
 * IOSWINE_NO_TUIKB_VALIDATE=1), when the class or a runtime symbol
 * is missing, or when the method cannot be found. Safe to call more
 * than once; the second call is a no-op. */
int tuikb_validate_install_hooks(void);

/* Statistics for the device log / host test. */
typedef struct {
    uint64_t installs;              /* successful installs (normally 1) */
    uint64_t calls;                 /* wrapper entries */
    uint64_t validations;           /* validate() runs (pre + post) */
    uint64_t corruptions;           /* mismatches found */
    uint64_t first_corrupt_offset;  /* offset of the first mismatch */
    uint64_t first_expected;        /* object the side table recorded */
    uint64_t first_got;             /* object the cache now returns */
} tuikb_validate_stats_t;
void tuikb_validate_stats(tuikb_validate_stats_t *out);

#ifdef TUIKB_VALIDATE_HOST_TEST
/* Host-test backend. The host test supplies fakes through this
 * struct (tuikb_validate_test_configure); on device the module
 * resolves the same symbols with dlsym + the PAC sign helper,
 * following cadisplay.c. dlsym_fn is consulted for every ObjC
 * runtime symbol by name (objc_getClass, sel_registerName,
 * class_getInstanceMethod, method_setImplementation,
 * method_getImplementation, objc_msgSend). getenv_fn answers the
 * env vars (NULL = unset). log_fn receives log bytes.
 * cache_lookup_fn is the dictionary lookup used by the validator:
 * it takes the cache object and an offset and returns the object
 * the cache holds for that offset (or NULL). On device it is the
 * real objectForKey: call (NSNumber key built from the offset); the
 * host test supplies a mock over a fake bucket array. */
typedef struct {
    void *(*dlsym_fn)(const char *name);
    const char *(*getenv_fn)(const char *name);
    void (*log_fn)(const char *msg, size_t len);
    void *(*cache_lookup_fn)(void *cache, uint64_t offset);
    void *cache;                    /* cache object passed to cache_lookup_fn */
} tuikb_validate_ops_t;

/* Host test only: install the ops table and reset all state. */
void tuikb_validate_test_configure(const tuikb_validate_ops_t *ops);
void tuikb_validate_test_reset(void);
/* Host test only: the currently installed IMP for objectForOffset:
 * (NULL when the class/method was never registered or the hooks
 * are not installed). Lets the test invoke the wrapper exactly as
 * the runtime would. */
void *tuikb_validate_test_imp(void);
/* Host test only: record a decoded (offset, object) pair and run
 * the validator against the configured cache. Returns 0 when every
 * recorded offset still returns its recorded object, 1 when a
 * mismatch was found (and logged). Exposed so the test can drive
 * the validation directly without the full wrapper. */
int tuikb_validate_test_record(uint64_t offset, void *obj);
int tuikb_validate_test_check(void);
#endif

#endif
