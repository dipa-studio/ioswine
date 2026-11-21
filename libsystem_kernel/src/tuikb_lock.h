/* tuikb_lock.h - serialize TUIKBGraphSerialization's decoded-object
 * cache.
 *
 * The cache is the _deserializationCache NSMutableDictionary, ivar +80
 * of TUIKBGraphSerialization (TextInputUI, 23G90). Its only accessor,
 * -[TUIKBGraphSerialization objectForOffset:], has no lock anywhere
 * in it (and the class has no lock ivar): lookup, decode and store
 * are all unguarded. Two threads decoding the same keyboard layout
 * graph at once race on the dictionary, which is the
 * ___NSMUTABLEDICTIONARY_CONCURRENT_MODIFICATION_DETECTED__ crash
 * and, once the dictionary storage is corrupted, the downstream
 * -[__NSArrayM insertObject:atIndex:] and objectForKey: faults
 *
 *
 * This module swizzles -[TUIKBGraphSerialization objectForOffset:]
 * to a wrapper that takes a recursive lock around the original IMP.
 * The lock must be recursive: objectForOffset: calls itself (and
 * the ref/array helpers that call it) for nested offsets, so the
 * same thread re-enters the wrapper on every nested decode. A
 * non-recursive lock would deadlock on the first nested object.
 *
 * Freestanding C11, no libc, no malloc, no __thread. Follows the
 * CADisplayLink swizzle pattern in cadisplay.c (dlsym via the
 * dyld gAPIs, PAC-sign the replacement IMP, all-or-nothing
 * install). No function-pointer tables in static data.
 *
 * No em dashes are used in this file.
 */
#ifndef TUIKB_LOCK_H
#define TUIKB_LOCK_H

#include <stdint.h>
#include <stddef.h>

/* Install the objectForOffset: wrapper. Returns 0 when the hooks
 * are installed (or were already installed), -1 when they are
 * disabled (IOSWINE_NO_TUIKB_LOCK=1), when the class or a runtime
 * symbol is missing, or when the method cannot be found. Safe to
 * call more than once; the second call is a no-op. */
int tuikb_lock_install_hooks(void);

/* Statistics for the device log / host test. */
typedef struct {
    uint64_t installs;       /* successful installs (normally 1) */
    uint64_t calls;          /* wrapper entries */
    uint64_t recursive;      /* wrapper entries that re-entered (depth > 0) */
    uint64_t contended;      /* entries that had to wait for another thread */
    uint64_t max_depth;      /* deepest recursion observed */
} tuikb_lock_stats_t;
void tuikb_lock_stats(tuikb_lock_stats_t *out);

#ifdef TUIKB_LOCK_HOST_TEST
/* Host-test backend operations. The host test supplies fakes
 * through this struct (tuikb_lock_test_configure); on device the
 * module resolves the same symbols with dlsym + the PAC sign
 * helper, following cadisplay.c. dlsym_fn is consulted for every
 * ObjC runtime symbol by name (objc_getClass, sel_registerName,
 * class_getInstanceMethod, method_setImplementation,
 * method_getImplementation). getenv_fn answers the kill-switch env
 * var (NULL = unset). log_fn receives log bytes. tid_fn returns the
 * current thread id (must differ between threads). futex_wait_fn
 * sleeps while *addr == val; futex_wake_fn wakes up to count
 * waiters on addr. */
typedef struct {
    void *(*dlsym_fn)(const char *name);
    const char *(*getenv_fn)(const char *name);
    void (*log_fn)(const char *msg, size_t len);
    long (*tid_fn)(void);
    long (*futex_wait_fn)(volatile int *addr, int val);
    long (*futex_wake_fn)(volatile int *addr, int count);
} tuikb_ops_t;

/* Host test only: install the ops table and reset all state. */
void tuikb_lock_test_configure(const tuikb_ops_t *ops);
void tuikb_lock_test_reset(void);
/* Host test only: the currently installed IMP for objectForOffset:
 * (NULL when the class/method was never registered or the hooks
 * are not installed). Lets the test invoke the wrapper exactly as
 * the runtime would. */
void *tuikb_lock_test_imp(void);
/* Host test only: disable (0) or enable (1) the lock inside the
 * wrapper. Disabled, the wrapper is a pure pass-through to the
 * original IMP, which is exactly the base-tree behaviour (no
 * serialization). The test uses this to show the race on base. */
void tuikb_lock_test_set_enabled(int enabled);
#endif

#endif
