/*
 * keyboard.c - Android presenter IME to UIKit text input (skeleton).
 *
 * Gated by IOSWINE_KEYBOARD=1.
 *
 * Path: presenter soft keyboard -> ISWS_MSG_TEXT / ISWS_MSG_KEY
 * (android/surface, @ioswine-surface) -> touch.c reader thread ->
 * keyboard_handle_event() -> UIKeyInput / UITextInput on the current
 * first responder. First responder tracking: hooks on UIResponder
 * -becomeFirstResponder / -resignFirstResponder, plus a fallback
 * walk of the key window hierarchy for isFirstResponder.
 *
 * What this skeleton deliberately does NOT do: no Java
 * InputConnection on the presenter side, no keyboard frame in the
 * show request (frame_h_pts is 0, unknown, in v1), no UITextInput
 * cursor movement (arrow keys are ignored), no autocorrection or
 * predictive text.
 *
 * Freestanding C11 for libsystem_kernel.dylib (arm64e, no libc
 * beyond <stdint.h>/<stddef.h>, no malloc, no __thread, no static
 * fn-ptr tables). ObjC/CF resolved via dlsym + lsk_sign_fn, like
 * touch.c. Depends on ISWS_MSG_TEXT, ISWS_MSG_KEYBOARD, isws_text_t,
 * PRESENT_EV_TEXT, and present_send_keyboard_request.
 */
#include "keyboard.h"
#include "present.h"

#ifndef KEYBOARD_HOST_TEST
#include "linux.h"
extern const char *lsk_getenv(const char *name);
extern int present_send_keyboard_request(uint32_t show, uint32_t traits,
                                         float frame_h_pts);
#endif

/* ------------------------------------------------------------------ */
/* Constants                                                          */
/* ------------------------------------------------------------------ */

#define KB_MAX_DELETE_PER_MSG   1024u
#define KB_MAX_TEXT_SCALARS     4096u
#define KB_MAX_HIERARCHY_DEPTH  8
#define KB_MAX_HIERARCHY_NODES  512

/* Pinned 23G90 cache addresses (same source as touch.c: dyld4::gAPIs
 * in libdyld (__TPRO_CONST)). Copied from touch.c, not re-derived. */
#define KB_GAPIS_ADDR           0x1ec954040ULL
#define KB_GAPIS_OBJ_DISC       0x63faULL
#define KB_SLOT_DLSYM           16ULL
#define KB_DIV_DLSYM            0x839dULL

/* Android keycodes (AKEYCODE_*, same numbering as ISWS_KEY_BACK). */
#define KB_ANDROID_KEY_ENTER    66u
#define KB_ANDROID_KEY_DEL      67u
#define KB_ANDROID_KEY_TAB      61u
#define KB_ANDROID_KEY_DPAD_LEFT  21u
#define KB_ANDROID_KEY_DPAD_RIGHT 22u
#define KB_ANDROID_KEY_DPAD_UP    19u
#define KB_ANDROID_KEY_DPAD_DOWN  20u
#define KB_ANDROID_KEY_ESC      111u

/* ------------------------------------------------------------------ */
/* Runtime state (zero-initialized in .bss)                           */
/* ------------------------------------------------------------------ */

#ifdef KEYBOARD_HOST_TEST
static const keyboard_test_ops_t *g_test_ops;
#endif

typedef struct {
    int resolved;
    void *(*objc_getClass)(const char *name);
    void *(*sel_registerName)(const char *name);
    void *objc_msgSend;
    void *(*objc_autoreleasePoolPush)(void);
    void (*objc_autoreleasePoolPop)(void *pool);
    void *(*object_getClass)(void *obj);
    void *(*class_getInstanceMethod)(void *cls, void *sel);
    void *(*method_setImplementation)(void *m, void *imp);
    void *sel_respondsToSelector;
    void *sel_sharedApplication;
    void *sel_keyWindow;
    void *sel_windows;
    void *sel_count;
    void *sel_objectAtIndex;
    void *sel_subviews;
    void *sel_isFirstResponder;
    void *sel_becomeFirstResponder;
    void *sel_resignFirstResponder;
    void *sel_hasText;
    void *sel_insertText;
    void *sel_deleteBackward;
    void *sel_setMarkedText;
    void *sel_unmarkText;
    void *sel_isSecureTextEntry;
    void *sel_autocorrectionType;
    void *sel_keyboardType;
    void *sel_returnKeyType;
    void *sel_stringWithUTF8String;
    void *sel_defaultCenter;
    void *sel_postNotification;
    void *note_will_show;   /* UIKeyboardWillShowNotification (NSString *) */
    void *note_did_show;    /* UIKeyboardDidShowNotification */
    void *note_will_hide;   /* UIKeyboardWillHideNotification */
    void *note_did_hide;    /* UIKeyboardDidHideNotification */
} kb_syms_t;

static kb_syms_t g_syms;
static void *g_first_responder;
static int g_keyboard_visible;
static int g_marked_active;
static int g_started;
static int g_trace;
static int g_hooks_installed;
static uint64_t g_dropped;
static uint64_t g_delivered_scalars;
static void *g_become_orig;
static void *g_resign_orig;

/* ------------------------------------------------------------------ */
/* Small freestanding helpers                                         */
/* ------------------------------------------------------------------ */

static size_t kb_strlen(const char *s) {
    size_t n = 0;
    if (s) while (s[n]) n++;
    return n;
}

static int kb_streq1(const char *s, char c) {
    return s && s[0] == c && s[1] == '\0';
}

static void kb_log_write(const char *buf, size_t len) {
    if (!buf || len == 0) return;
#ifdef KEYBOARD_HOST_TEST
    if (g_test_ops && g_test_ops->log_fn) {
        g_test_ops->log_fn(buf, len);
    }
#else
    sys_write(2, buf, len);
#endif
}

static void kb_log_str(const char *s) {
    kb_log_write(s, kb_strlen(s));
}

static void kb_trace_str(const char *s) {
    if (g_trace) kb_log_str(s);
}

static const char *kb_getenv(const char *name) {
#ifdef KEYBOARD_HOST_TEST
    if (g_test_ops && g_test_ops->getenv_fn) return g_test_ops->getenv_fn(name);
    return NULL;
#else
    return lsk_getenv(name);
#endif
}

/* ------------------------------------------------------------------ */
/* UTF-8 validation (strict)                                          */
/* ------------------------------------------------------------------ */

int keyboard_validate_utf8(const char *utf8, size_t len) {
    size_t i = 0;
    int count = 0;
    if (!utf8) return -1;
    if (len == 0) return -1;
    while (i < len) {
        unsigned char c = (unsigned char)utf8[i];
        uint32_t cp;
        size_t extra;
        uint32_t min_cp;
        if (c == 0x00) return -1;
        if (c < 0x80) {
            i++;
            count++;
            continue;
        }
        if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; min_cp = 0x80; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; min_cp = 0x800; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; min_cp = 0x10000; }
        else return -1;
        if (i + extra >= len) return -1;
        for (size_t j = 1; j <= extra; j++) {
            unsigned char cc = (unsigned char)utf8[i + j];
            if ((cc & 0xC0) != 0x80) return -1;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (cp < min_cp) return -1;              /* overlong form */
        if (cp >= 0xD800 && cp <= 0xDFFF) return -1; /* surrogate */
        if (cp > 0x10FFFF) return -1;
        i += extra + 1;
        count++;
        if (count > (int)KB_MAX_TEXT_SCALARS) return -1;
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* DSC dlsym / dlopen + arm64e PAC signing (mirrors touch.c)          */
/* ------------------------------------------------------------------ */

static void *kb_sign_fn(void *p) {
    if (!p) return NULL;
#if defined(__aarch64__) && !defined(KEYBOARD_HOST_TEST)
    uint64_t v = (uint64_t)p;
    __asm__ volatile(
        "xpaci %0\n"
        "paciza %0\n"
        : "+r"(v)
    );
    return (void *)v;
#else
    return p;
#endif
}

#if defined(__aarch64__) && !defined(KEYBOARD_HOST_TEST)
static __attribute__((noinline)) void *kb_gapis_call2(uint64_t slot, uint64_t div,
                                                       uint64_t arg1, uint64_t arg2) {
    uint64_t obj = *(volatile uint64_t *)KB_GAPIS_ADDR;
    if (!obj) return NULL;
    uint64_t vt = *(volatile uint64_t *)obj;
    uint64_t obj_div = (obj & 0x0000ffffffffffffULL) | (KB_GAPIS_OBJ_DISC << 48);
    __asm__ volatile("autda %0, %1" : "+r"(vt) : "r"(obj_div));
    uint64_t slot_addr = vt + slot * 8ULL;
    uint64_t fn = *(volatile uint64_t *)slot_addr;
    uint64_t fn_div = (slot_addr & 0x0000ffffffffffffULL) | (div << 48);

    register uint64_t x0 __asm__("x0") = obj;
    register uint64_t x1 __asm__("x1") = arg1;
    register uint64_t x2 __asm__("x2") = arg2;
    register uint64_t x16 __asm__("x16") = fn;
    register uint64_t x17 __asm__("x17") = fn_div;
    __asm__ volatile(
        "blraa x16, x17"
        : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x16), "+r"(x17)
        :
        : "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
          "x12", "x13", "x14", "x15", "x30",
          "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
          "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
          "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31",
          "cc", "memory"
    );
    return (void *)x0;
}
#endif

static void *kb_dlsym(const char *name) {
    if (!name) return NULL;
#ifdef KEYBOARD_HOST_TEST
    if (g_test_ops && g_test_ops->dlsym_default) {
        return g_test_ops->dlsym_default(name);
    }
    return NULL;
#elif defined(__aarch64__)
    return kb_gapis_call2(KB_SLOT_DLSYM, KB_DIV_DLSYM,
                          (uint64_t)(intptr_t)-2 /* RTLD_DEFAULT */, (uint64_t)name);
#else
    return NULL;
#endif
}

/* Notification name data symbols live in UIKitCore; dlsym returns the
 * address of the variable, so dereference once to get the NSString. */
static void *kb_notif_name(const char *sym) {
    void *addr = kb_dlsym(sym);
    if (!addr) return NULL;
#ifdef KEYBOARD_HOST_TEST
    return addr;
#else
    return *(void **)addr;
#endif
}

/* ------------------------------------------------------------------ */
/* Symbol resolution                                                  */
/* ------------------------------------------------------------------ */

static int kb_ensure_syms(void) {
    if (g_syms.resolved) {
        return (g_syms.objc_getClass && g_syms.sel_registerName &&
                g_syms.objc_msgSend) ? 0 : -1;
    }
    g_syms.resolved = 1;

    g_syms.objc_getClass = (void *(*)(const char *))kb_sign_fn(kb_dlsym("objc_getClass"));
    g_syms.sel_registerName = (void *(*)(const char *))kb_sign_fn(kb_dlsym("sel_registerName"));
    g_syms.objc_msgSend = kb_sign_fn(kb_dlsym("objc_msgSend"));
    g_syms.objc_autoreleasePoolPush =
        (void *(*)(void))kb_sign_fn(kb_dlsym("objc_autoreleasePoolPush"));
    g_syms.objc_autoreleasePoolPop =
        (void (*)(void *))kb_sign_fn(kb_dlsym("objc_autoreleasePoolPop"));
    g_syms.object_getClass = (void *(*)(void *))kb_sign_fn(kb_dlsym("object_getClass"));
    g_syms.class_getInstanceMethod =
        (void *(*)(void *, void *))kb_sign_fn(kb_dlsym("class_getInstanceMethod"));
    g_syms.method_setImplementation =
        (void *(*)(void *, void *))kb_sign_fn(kb_dlsym("method_setImplementation"));

    if (g_syms.sel_registerName) {
        void *(*sr)(const char *) = g_syms.sel_registerName;
        g_syms.sel_respondsToSelector = sr("respondsToSelector:");
        g_syms.sel_sharedApplication = sr("sharedApplication");
        g_syms.sel_keyWindow = sr("keyWindow");
        g_syms.sel_windows = sr("windows");
        g_syms.sel_count = sr("count");
        g_syms.sel_objectAtIndex = sr("objectAtIndex:");
        g_syms.sel_subviews = sr("subviews");
        g_syms.sel_isFirstResponder = sr("isFirstResponder");
        g_syms.sel_becomeFirstResponder = sr("becomeFirstResponder");
        g_syms.sel_resignFirstResponder = sr("resignFirstResponder");
        g_syms.sel_hasText = sr("hasText");
        g_syms.sel_insertText = sr("insertText:");
        g_syms.sel_deleteBackward = sr("deleteBackward");
        g_syms.sel_setMarkedText = sr("setMarkedText:selectedRange:");
        g_syms.sel_unmarkText = sr("unmarkText");
        g_syms.sel_isSecureTextEntry = sr("isSecureTextEntry");
        g_syms.sel_autocorrectionType = sr("autocorrectionType");
        g_syms.sel_keyboardType = sr("keyboardType");
        g_syms.sel_returnKeyType = sr("returnKeyType");
        g_syms.sel_stringWithUTF8String = sr("stringWithUTF8String:");
        g_syms.sel_defaultCenter = sr("defaultCenter");
        g_syms.sel_postNotification = sr("postNotificationName:object:userInfo:");
    }

    /* Best effort: notification names may be absent in the host test
     * or in a stripped cache; posting is skipped if any is NULL. */
    g_syms.note_will_show = kb_notif_name("UIKeyboardWillShowNotification");
    g_syms.note_did_show = kb_notif_name("UIKeyboardDidShowNotification");
    g_syms.note_will_hide = kb_notif_name("UIKeyboardWillHideNotification");
    g_syms.note_did_hide = kb_notif_name("UIKeyboardDidHideNotification");

    return (g_syms.objc_getClass && g_syms.sel_registerName &&
            g_syms.objc_msgSend) ? 0 : -1;
}

static inline uint8_t kb_responds(void *obj, void *sel) {
    if (!obj || !sel || !g_syms.objc_msgSend || !g_syms.sel_respondsToSelector) {
        return 0;
    }
    uint8_t (*fn)(void *, void *, void *) =
        (uint8_t (*)(void *, void *, void *))g_syms.objc_msgSend;
    return fn(obj, g_syms.sel_respondsToSelector, sel) ? 1 : 0;
}

static void *kb_shared_app(void) {
    void *cls;
    if (kb_ensure_syms() != 0) return NULL;
    cls = g_syms.objc_getClass("UIApplication");
    if (!cls || !g_syms.sel_sharedApplication) return NULL;
    if (!kb_responds(cls, g_syms.sel_sharedApplication)) return NULL;
    return ((void *(*)(void *, void *))g_syms.objc_msgSend)(cls, g_syms.sel_sharedApplication);
}

/* ------------------------------------------------------------------ */
/* Notification posting (best effort, NULL userInfo in v1)            */
/* ------------------------------------------------------------------ */

static void kb_post_notification(void *name) {
    void *center_cls;
    void *center;
    if (!name) return;
    if (kb_ensure_syms() != 0) return;
    if (!g_syms.sel_defaultCenter || !g_syms.sel_postNotification) return;
    center_cls = g_syms.objc_getClass("NSNotificationCenter");
    if (!center_cls) return;
    if (!kb_responds(center_cls, g_syms.sel_defaultCenter)) return;
    center = ((void *(*)(void *, void *))g_syms.objc_msgSend)(center_cls, g_syms.sel_defaultCenter);
    if (!center) return;
    ((void (*)(void *, void *, void *, void *, void *))g_syms.objc_msgSend)(
        center, g_syms.sel_postNotification, name, g_first_responder, NULL);
}

/* ------------------------------------------------------------------ */
/* First responder tracking                                           */
/* ------------------------------------------------------------------ */

static int kb_is_text_responder(void *obj) {
    if (!obj) return 0;
    return kb_responds(obj, g_syms.sel_insertText) &&
           kb_responds(obj, g_syms.sel_deleteBackward) &&
           kb_responds(obj, g_syms.sel_hasText);
}

/* Depth-first walk of a view subtree looking for isFirstResponder.
 * Returns the responder or NULL. Bounded: depth cap and node cap. */
static void *kb_find_first_responder_in(void *view, int depth, int *nodes) {
    void *subs;
    uint64_t n;
    uint64_t i;
    if (!view || depth > KB_MAX_HIERARCHY_DEPTH || !nodes) return NULL;
    if (*nodes >= KB_MAX_HIERARCHY_NODES) return NULL;
    (*nodes)++;
    if (g_syms.sel_isFirstResponder && kb_responds(view, g_syms.sel_isFirstResponder)) {
        uint8_t (*fn)(void *, void *) =
            (uint8_t (*)(void *, void *))g_syms.objc_msgSend;
        if (fn(view, g_syms.sel_isFirstResponder)) return view;
    }
    if (!g_syms.sel_subviews || !kb_responds(view, g_syms.sel_subviews)) return NULL;
    subs = ((void *(*)(void *, void *))g_syms.objc_msgSend)(view, g_syms.sel_subviews);
    if (!subs || !g_syms.sel_count || !kb_responds(subs, g_syms.sel_count)) return NULL;
    n = (uint64_t)(uintptr_t)((void *(*)(void *, void *))g_syms.objc_msgSend)(subs, g_syms.sel_count);
    if (n > 1024) n = 1024;
    for (i = 0; i < n; i++) {
        void *child;
        void *found;
        if (!g_syms.sel_objectAtIndex || !kb_responds(subs, g_syms.sel_objectAtIndex)) break;
        child = ((void *(*)(void *, void *, uint64_t))g_syms.objc_msgSend)(
            subs, g_syms.sel_objectAtIndex, i);
        found = kb_find_first_responder_in(child, depth + 1, nodes);
        if (found) return found;
    }
    return NULL;
}

void *keyboard_refresh_first_responder(void) {
    void *app;
    void *win;
    void *found;
    int nodes;
    if (kb_ensure_syms() != 0) return NULL;
    app = kb_shared_app();
    if (!app) return NULL;
    win = NULL;
    if (g_syms.sel_keyWindow && kb_responds(app, g_syms.sel_keyWindow)) {
        win = ((void *(*)(void *, void *))g_syms.objc_msgSend)(app, g_syms.sel_keyWindow);
    }
    if (!win && g_syms.sel_windows && kb_responds(app, g_syms.sel_windows)) {
        void *wins = ((void *(*)(void *, void *))g_syms.objc_msgSend)(app, g_syms.sel_windows);
        if (wins && g_syms.sel_count && kb_responds(wins, g_syms.sel_count)) {
            uint64_t n = (uint64_t)(uintptr_t)((void *(*)(void *, void *))g_syms.objc_msgSend)(
                wins, g_syms.sel_count);
            if (n > 0 && g_syms.sel_objectAtIndex && kb_responds(wins, g_syms.sel_objectAtIndex)) {
                win = ((void *(*)(void *, void *, uint64_t))g_syms.objc_msgSend)(
                    wins, g_syms.sel_objectAtIndex, 0);
            }
        }
    }
    if (!win) return NULL;
    nodes = 0;
    found = kb_find_first_responder_in(win, 0, &nodes);
    if (found) g_first_responder = found;
    return found;
}

/* Read UITextInputTraits from the responder (best effort, defaults
 * when a selector is absent). */
static uint32_t kb_read_traits(void *obj) {
    uint32_t traits = 0;
    if (!obj) return 0;
    if (g_syms.sel_isSecureTextEntry && kb_responds(obj, g_syms.sel_isSecureTextEntry)) {
        uint8_t (*fn)(void *, void *) = (uint8_t (*)(void *, void *))g_syms.objc_msgSend;
        if (fn(obj, g_syms.sel_isSecureTextEntry)) traits |= KB_TRAIT_SECURE;
    }
    if (g_syms.sel_autocorrectionType && kb_responds(obj, g_syms.sel_autocorrectionType)) {
        /* UITextAutocorrectionType: 0 default, 1 no, 2 yes. */
        int64_t (*fn)(void *, void *) = (int64_t (*)(void *, void *))g_syms.objc_msgSend;
        if (fn(obj, g_syms.sel_autocorrectionType) == 1) traits |= KB_TRAIT_NO_AUTOCORRECT;
    }
    if (g_syms.sel_keyboardType && kb_responds(obj, g_syms.sel_keyboardType)) {
        int64_t (*fn)(void *, void *) = (int64_t (*)(void *, void *))g_syms.objc_msgSend;
        int64_t t = fn(obj, g_syms.sel_keyboardType);
        if (t >= 0 && t <= 15) traits |= ((uint32_t)t << KB_TRAIT_KB_TYPE_SHIFT);
    }
    if (g_syms.sel_returnKeyType && kb_responds(obj, g_syms.sel_returnKeyType)) {
        int64_t (*fn)(void *, void *) = (int64_t (*)(void *, void *))g_syms.objc_msgSend;
        int64_t t = fn(obj, g_syms.sel_returnKeyType);
        if (t >= 0 && t <= 15) traits |= ((uint32_t)t << KB_TRAIT_RETURN_SHIFT);
    }
    return traits;
}

int keyboard_request_show(int show) {
    uint32_t traits;
    int rc;
    if (kb_ensure_syms() != 0) return -1;
    traits = kb_read_traits(g_first_responder);
#ifdef KEYBOARD_HOST_TEST
    if (g_test_ops && g_test_ops->send_keyboard_fn) {
        rc = g_test_ops->send_keyboard_fn(show ? 1u : 0u, traits, 0.0f);
        if (rc == 0) g_keyboard_visible = show ? 1 : 0;
        return rc;
    }
    return -1;
#else
    rc = present_send_keyboard_request(show ? 1u : 0u, traits, 0.0f);
    if (rc == 0) g_keyboard_visible = show ? 1 : 0;
    return rc;
#endif
}

void keyboard_note_first_responder(void *responder) {
    int was_text;
    int is_text;
    if (kb_ensure_syms() != 0) {
        g_first_responder = responder;
        return;
    }
    was_text = kb_is_text_responder(g_first_responder);
    g_first_responder = responder;
    is_text = kb_is_text_responder(responder);
    if (is_text && !was_text) {
        kb_trace_str("[keyboard] first responder is text input, requesting show\n");
        kb_post_notification(g_syms.note_will_show);
        (void)keyboard_request_show(1);
        kb_post_notification(g_syms.note_did_show);
    } else if (!is_text && was_text) {
        kb_trace_str("[keyboard] text input resigned, requesting hide\n");
        kb_post_notification(g_syms.note_will_hide);
        (void)keyboard_request_show(0);
        kb_post_notification(g_syms.note_did_hide);
    }
}

/* ------------------------------------------------------------------ */
/* becomeFirstResponder / resignFirstResponder hooks                  */
/* ------------------------------------------------------------------ */

typedef uint8_t (*kb_bool_msg_t)(void *self, void *cmd);

static uint8_t kb_become_hook(void *self, void *cmd) {
    uint8_t r = 1;
    kb_bool_msg_t orig = (kb_bool_msg_t)g_become_orig;
    if (orig) r = orig(self, cmd);
    if (r) keyboard_note_first_responder(self);
    return r;
}

static uint8_t kb_resign_hook(void *self, void *cmd) {
    uint8_t r = 1;
    kb_bool_msg_t orig = (kb_bool_msg_t)g_resign_orig;
    if (orig) r = orig(self, cmd);
    if (r && g_first_responder == self) keyboard_note_first_responder(NULL);
    return r;
}

static void kb_install_hooks(void) {
    void *cls;
    void *m;
    if (g_hooks_installed) return;
    g_hooks_installed = 1;
    if (kb_ensure_syms() != 0) return;
    if (!g_syms.class_getInstanceMethod || !g_syms.method_setImplementation ||
        !g_syms.object_getClass || !g_syms.objc_getClass) return;
    cls = g_syms.objc_getClass("UIResponder");
    if (!cls) return;
    if (g_syms.sel_becomeFirstResponder) {
        m = g_syms.class_getInstanceMethod(cls, g_syms.sel_becomeFirstResponder);
        if (m) {
            g_become_orig = g_syms.method_setImplementation(
                m, (void *)kb_sign_fn((void *)kb_become_hook));
        }
    }
    if (g_syms.sel_resignFirstResponder) {
        m = g_syms.class_getInstanceMethod(cls, g_syms.sel_resignFirstResponder);
        if (m) {
            g_resign_orig = g_syms.method_setImplementation(
                m, (void *)kb_sign_fn((void *)kb_resign_hook));
        }
    }
    kb_trace_str("[keyboard] first-responder hooks installed\n");
}

/* ------------------------------------------------------------------ */
/* Text delivery                                                      */
/* ------------------------------------------------------------------ */

/* Build an NSString from UTF-8. Returns NULL when the runtime is
 * unavailable; the caller treats that as a failed delivery. */
static void *kb_nsstring_from_utf8(const char *utf8, size_t len) {
    void *cls;
    char *tmp;
    /* NSString +stringWithUTF8String: needs a NUL-terminated C
     * string. Validated UTF-8 never contains NUL, so copy into a
     * stack buffer and terminate it. The scalar cap in
     * keyboard_validate_utf8 (4096 scalars, at most 4 bytes each)
     * bounds the copy at 16384 bytes. */
    char buf[16384];
    size_t i;
    if (!utf8 || len == 0 || len >= sizeof(buf)) return NULL;
    if (kb_ensure_syms() != 0) return NULL;
    cls = g_syms.objc_getClass("NSString");
    if (!cls || !g_syms.sel_stringWithUTF8String) return NULL;
    for (i = 0; i < len; i++) buf[i] = utf8[i];
    buf[len] = '\0';
    tmp = buf;
    if (!kb_responds(cls, g_syms.sel_stringWithUTF8String)) return NULL;
    return ((void *(*)(void *, void *, const char *))g_syms.objc_msgSend)(
        cls, g_syms.sel_stringWithUTF8String, tmp);
}

int keyboard_commit_text(const char *utf8, size_t len) {
    int scalars;
    void *ns;
    void *pool;
    if (!utf8 || len == 0) return -1;
    scalars = keyboard_validate_utf8(utf8, len);
    if (scalars < 0) return -1;
    if (kb_ensure_syms() != 0) return -1;
    if (!kb_is_text_responder(g_first_responder)) {
        g_dropped++;
        kb_trace_str("[keyboard] commit dropped: no text first responder\n");
        return 0;
    }
    if (g_marked_active) {
        (void)keyboard_unmark_text();
    }
    ns = kb_nsstring_from_utf8(utf8, len);
    if (!ns) return -1;
    pool = g_syms.objc_autoreleasePoolPush ? g_syms.objc_autoreleasePoolPush() : NULL;
    ((void (*)(void *, void *, void *))g_syms.objc_msgSend)(
        g_first_responder, g_syms.sel_insertText, ns);
    if (pool && g_syms.objc_autoreleasePoolPop) g_syms.objc_autoreleasePoolPop(pool);
    g_delivered_scalars += (uint64_t)scalars;
    kb_trace_str("[keyboard] committed text\n");
    return scalars;
}

int keyboard_set_marked_text(const char *utf8, size_t len) {
    int scalars;
    void *ns;
    void *pool;
    typedef struct { size_t location; size_t length; } kb_range_t;
    kb_range_t range;
    if (!utf8 || len == 0) return -1;
    scalars = keyboard_validate_utf8(utf8, len);
    if (scalars < 0) return -1;
    if (kb_ensure_syms() != 0) return -1;
    if (!kb_is_text_responder(g_first_responder)) {
        g_dropped++;
        kb_trace_str("[keyboard] marked dropped: no text first responder\n");
        return 0;
    }
    if (!kb_responds(g_first_responder, g_syms.sel_setMarkedText)) {
        /* Plain UIKeyInput only: commit directly, no composition. */
        kb_trace_str("[keyboard] no setMarkedText, committing directly\n");
        return keyboard_commit_text(utf8, len);
    }
    ns = kb_nsstring_from_utf8(utf8, len);
    if (!ns) return -1;
    /* Caret at the end of the marked text [guess]: selectedRange
     * {markedLength, 0}. */
    range.location = (size_t)scalars;
    range.length = 0;
    pool = g_syms.objc_autoreleasePoolPush ? g_syms.objc_autoreleasePoolPush() : NULL;
    ((void (*)(void *, void *, void *, kb_range_t))g_syms.objc_msgSend)(
        g_first_responder, g_syms.sel_setMarkedText, ns, range);
    if (pool && g_syms.objc_autoreleasePoolPop) g_syms.objc_autoreleasePoolPop(pool);
    g_marked_active = 1;
    kb_trace_str("[keyboard] marked text set\n");
    return scalars;
}

int keyboard_unmark_text(void) {
    if (kb_ensure_syms() != 0) return -1;
    if (!g_marked_active) return 0;
    g_marked_active = 0;
    if (!kb_is_text_responder(g_first_responder)) return 0;
    if (!kb_responds(g_first_responder, g_syms.sel_unmarkText)) return 0;
    ((void (*)(void *, void *))g_syms.objc_msgSend)(
        g_first_responder, g_syms.sel_unmarkText);
    kb_trace_str("[keyboard] unmarked text\n");
    return 0;
}

int keyboard_delete_backward(uint32_t count) {
    uint32_t done = 0;
    uint32_t n;
    if (kb_ensure_syms() != 0) return 0;
    if (!kb_is_text_responder(g_first_responder)) {
        g_dropped++;
        return 0;
    }
    if (count == 0) return 0;
    if (count > KB_MAX_DELETE_PER_MSG) count = KB_MAX_DELETE_PER_MSG;
    for (n = 0; n < count; n++) {
        if (g_syms.sel_hasText && kb_responds(g_first_responder, g_syms.sel_hasText)) {
            uint8_t (*fn)(void *, void *) =
                (uint8_t (*)(void *, void *))g_syms.objc_msgSend;
            if (!fn(g_first_responder, g_syms.sel_hasText)) break;
        }
        ((void (*)(void *, void *))g_syms.objc_msgSend)(
            g_first_responder, g_syms.sel_deleteBackward);
        done++;
    }
    kb_trace_str("[keyboard] deleteBackward\n");
    return (int)done;
}

/* ------------------------------------------------------------------ */
/* Hardware keys                                                      */
/* ------------------------------------------------------------------ */

int keyboard_handle_key(const isws_key_t *k) {
    if (!k) return -1;
    if (k->action != ISWS_KEY_ACTION_DOWN) return 1; /* up: ignored */
    if (k->keycode == ISWS_KEY_BACK) {
        /* Back: dismiss the keyboard first; the app never sees it
         * as a text event. */
        if (g_keyboard_visible) {
            (void)keyboard_request_show(0);
            if (g_first_responder && g_syms.sel_resignFirstResponder &&
                kb_responds(g_first_responder, g_syms.sel_resignFirstResponder)) {
                ((void (*)(void *, void *))g_syms.objc_msgSend)(
                    g_first_responder, g_syms.sel_resignFirstResponder);
            }
        }
        return 0;
    }
    if (k->keycode == KB_ANDROID_KEY_DEL) {
        (void)keyboard_delete_backward(1);
        return 0;
    }
    if (k->keycode == KB_ANDROID_KEY_ENTER) {
        (void)keyboard_commit_text("\n", 1);
        return 0;
    }
    if (k->keycode == KB_ANDROID_KEY_TAB) {
        /* Tab focus navigation is not implemented in v1; insert a
         * tab character when a text field is focused, ignore
         * otherwise. */
        if (kb_is_text_responder(g_first_responder)) {
            (void)keyboard_commit_text("\t", 1);
            return 0;
        }
        return 1;
    }
    if (k->keycode == KB_ANDROID_KEY_DPAD_LEFT ||
        k->keycode == KB_ANDROID_KEY_DPAD_RIGHT ||
        k->keycode == KB_ANDROID_KEY_DPAD_UP ||
        k->keycode == KB_ANDROID_KEY_DPAD_DOWN) {
        /* Cursor movement needs UITextInput selectedTextRange
         * manipulation; not implemented in v1. */
        kb_trace_str("[keyboard] arrow key ignored (no cursor support in v1)\n");
        return 1;
    }
    if (k->keycode == KB_ANDROID_KEY_ESC) {
        if (g_keyboard_visible) {
            (void)keyboard_request_show(0);
            return 0;
        }
        return 1;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Event dispatch                                                     */
/* ------------------------------------------------------------------ */

int keyboard_handle_event(const present_event_t *ev) {
    const isws_text_t *t;
    if (!ev) return -1;
    if (ev->kind == PRESENT_EV_TEXT) {
        t = &ev->u.text;
        if (t->kind == KB_TEXT_COMMIT) {
            if (!t->utf8 || t->utf8_len == 0) return -1;
            (void)keyboard_commit_text((const char *)t->utf8, t->utf8_len);
            return 0;
        }
        if (t->kind == KB_TEXT_MARKED) {
            if (!t->utf8 || t->utf8_len == 0) return -1;
            (void)keyboard_set_marked_text((const char *)t->utf8, t->utf8_len);
            return 0;
        }
        if (t->kind == KB_TEXT_UNMARK) {
            (void)keyboard_unmark_text();
            return 0;
        }
        if (t->kind == KB_TEXT_DELETE) {
            (void)keyboard_delete_backward(t->count);
            return 0;
        }
        return 1;
    }
    if (ev->kind == PRESENT_EV_KEY) {
        return keyboard_handle_key(&ev->u.key);
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Init                                                               */
/* ------------------------------------------------------------------ */

void keyboard_init(void) {
    const char *en;
    const char *tr;
    if (g_started) return;
    en = kb_getenv("IOSWINE_KEYBOARD");
    if (!en || !kb_streq1(en, '1')) return;
    tr = kb_getenv("IOSWINE_KEYBOARD_TRACE");
    g_trace = (tr && kb_streq1(tr, '1')) ? 1 : 0;
    g_started = 1;
    if (kb_ensure_syms() != 0) {
        kb_log_str("[keyboard] init failed: missing ObjC symbols\n");
        return;
    }
    kb_install_hooks();
    (void)keyboard_refresh_first_responder();
    kb_log_str("[keyboard] started\n");
}

int keyboard_is_visible(void) {
    return g_keyboard_visible;
}

uint64_t keyboard_dropped_count(void) {
    return g_dropped;
}

#ifdef KEYBOARD_HOST_TEST
void keyboard_test_configure(const keyboard_test_ops_t *ops) {
    g_test_ops = ops;
}

void keyboard_test_reset(void) {
    kb_syms_t zero;
    size_t i;
    const uint8_t *p = (const uint8_t *)&zero;
    uint8_t *q = (uint8_t *)&g_syms;
    for (i = 0; i < sizeof(zero); i++) q[i] = 0;
    (void)p;
    g_first_responder = NULL;
    g_keyboard_visible = 0;
    g_marked_active = 0;
    g_started = 0;
    g_trace = 0;
    g_hooks_installed = 0;
    g_dropped = 0;
    g_delivered_scalars = 0;
    g_become_orig = NULL;
    g_resign_orig = NULL;
}

void *keyboard_test_first_responder(void) {
    return g_first_responder;
}

int keyboard_test_marked_active(void) {
    return g_marked_active;
}
#endif
