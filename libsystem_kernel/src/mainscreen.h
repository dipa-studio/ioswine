/*
 * mainscreen.h - hook for +[UIScreen mainScreen].
 *
 * The boot race: +[UIScreen mainScreen] throws
 * NSInternalInconsistencyException "returning nil screen from
 * mainScreen is not allowed!" when +[UIScreen screens] is empty.
 * The screens array gets its first entry either from the CoreAnimation
 * display path (-[UIScreen _mainDisplayInit] -> +[CADisplay
 * mainDisplay] -> CA::Display::ensure_displays -> query_displays ->
 * MIG 40231 _CASGetDisplays to the render server) or from the
 * FrontBoard scene (the scene's displayConfiguration creates the
 * UIScreen). On this system the render server never answers 40231,
 * so query_displays retries for 5 s and then gives up, ensure_displays
 * latches its done flag, +[CADisplay mainDisplay] returns NULL from
 * the primary-display slot [0x1ecd99000 + 1488] forever, and the only
 * thing that can ever populate screens is the scene, which arrives
 * asynchronously (fakeboard queues the scene-create callout at
 * initializeClient completion, delivered later via ipc_svc_pump).
 * A mainScreen call that lands after the 5 s latch and before the
 * scene registers throws. That is the 4/11 boot crash.
 *
 * The fix: swizzle +[UIScreen mainScreen] (same method_setImplementation
 * pattern cadisplay.c uses for CADisplayLink) so that, when no scene
 * has been created yet, the call waits (bounded, 5 s, matching the
 * CA deadline) for the first scene instead of throwing immediately,
 * then waits briefly (500 ms) for screens to become non-empty, and
 * only then calls through to the original implementation. When a
 * scene is already present the hook is a straight pass-through.
 * When fakeboard is not inited no scene will ever come, so the hook
 * calls through immediately and never delays the call.
 *
 * Install: mainscreen_install_hooks() is all-or-nothing and safe to
 * call early (it no-ops when the runtime symbols or the UIScreen
 * class are missing). Call it from __libkernel_init (before app main,
 * so it is in place before the first mainScreen call, which can
 * precede __libkernel_init_late) and retry it from
 * __libkernel_init_late and at the 200th svc send, mirroring
 * cadisplay_link_install_hooks. IOSWINE_NO_MAINSCREEN=1 disables it.
 *
 * No em dashes are used in this file.
 */
#ifndef IOSWINE_MAINSCREEN_H
#define IOSWINE_MAINSCREEN_H

#include <stdint.h>
#include <stddef.h>

/* Install the +[UIScreen mainScreen] hook. Returns 0 when the hook
 * is installed (or was already installed), -1 when the runtime
 * symbols, the UIScreen class, or the method are missing, or when
 * disabled by IOSWINE_NO_MAINSCREEN=1. Safe to call repeatedly;
 * the second and later calls are no-ops. */
int mainscreen_install_hooks(void);

/* 1 when the hook is installed, else 0. Diagnostic only. */
int mainscreen_hooks_installed(void);

#ifdef MAINSCREEN_HOST_TEST
/* Host-test backend operations. The host test supplies fakes
 * through this struct (mainscreen_test_configure); on device the
 * code resolves the same symbols with dlsym + lsk_sign_fn,
 * following cadisplay.c and keyboard.c. dlsym_fn is consulted for
 * every ObjC symbol by name (objc_getClass, sel_registerName,
 * class_getClassMethod, method_setImplementation, objc_msgSend).
 * scene_state_fn returns the fakeboard scene state
 * (FB_SCENE_NONE / CREATING / FOREGROUND / BACKGROUND /
 * INVALIDATED, fakeboard.h). scene_expected_fn returns 1 when a
 * scene can still arrive (fakeboard inited), else 0.
 * sleep_ms_fn sleeps the given milliseconds (the test supplies a
 * real short sleep; on device this is nanosleep). getenv_fn
 * answers the kill-switch env var (NULL = unset). log_fn receives
 * log bytes. */
typedef struct {
    void *(*dlsym_fn)(const char *name);
    int (*scene_state_fn)(void);
    int (*scene_expected_fn)(void);
    void (*sleep_ms_fn)(uint32_t ms);
    const char *(*getenv_fn)(const char *name);
    void (*log_fn)(const char *msg, size_t len);
} ms_ops_t;

/* Host test only: install the ops table and reset all state. */
void mainscreen_test_configure(const ms_ops_t *ops);
void mainscreen_test_reset(void);
/* Host test only: the currently installed IMP for +[UIScreen
 * mainScreen], or NULL when the hook is not installed. Lets the
 * test invoke the hook exactly as the runtime would. */
void *mainscreen_test_imp(void);
/* Host test only: the saved original IMP (the mock "real"
 * mainScreen), or NULL when the hook is not installed. */
void *mainscreen_test_orig(void);
#endif /* MAINSCREEN_HOST_TEST */

#endif /* IOSWINE_MAINSCREEN_H */
