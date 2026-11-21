/*
 * keyboard_test.c - Host unit tests for keyboard.c + ISWS_MSG_TEXT /
 * ISWS_MSG_KEYBOARD protocol round-trip.
 *
 * Plain cc + ASan/UBSan on Linux host. Tests:
 *   1. Strict UTF-8 validation (valid multi-byte, overlong,
 *      surrogates, truncation, NUL, > U+10FFFF)
 *   2. ISWS_MSG_TEXT / ISWS_MSG_KEYBOARD encode/decode round-trip
 *      and rejection of malformed payloads
 *   3. IOSWINE_KEYBOARD=1 gating and first-responder hook install
 *   4. becomeFirstResponder / resignFirstResponder hooks: keyboard
 *      show/hide requests with traits, keyboard notifications
 *   5. insertText: ordering, multi-byte and emoji commits, invalid
 *      input rejected, no-responder drop counting
 *   6. Marked text then commit (unmark commits marked text first),
 *      plain-UIKeyInput fallback to direct commit
 *   7. deleteBackward: order, empty-buffer no-op, hasText early stop
 *   8. Hardware keys: DEL / ENTER / TAB / BACK / arrows / key-up
 *   9. First-responder discovery by walking the key window
 *  10. keyboard_handle_event() dispatch for TEXT and KEY events
 *  11. Fuzz: random bytes through the UTF-8 validator, random valid
 *      UTF-8 through commit
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keyboard.h"
#include "present.h"
#include "protocol.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* ------------------------------------------------------------------ */
/* Fake ObjC runtime                                                  */
/* ------------------------------------------------------------------ */

#define KIND_APP 1
#define KIND_WINDOW 2
#define KIND_VIEW_TEXT 3
#define KIND_VIEW_PLAIN 4
#define KIND_NOTIF_CENTER 5
#define KIND_NSSTRING 6
#define KIND_CLASS 7
#define KIND_ARRAY 8

typedef struct fake_obj {
    int kind;
} fake_obj_t;

typedef struct {
    int kind; /* KIND_NSSTRING */
    char buf[2048];
    size_t len;
} fake_nsstring_t;

#define MAX_CALL_LOG 64
#define MAX_NOTIF_LOG 16

typedef struct {
    int kind; /* KIND_VIEW_TEXT or KIND_VIEW_PLAIN */
    int is_first_responder;
    int supports_marked; /* UITextInput vs plain UIKeyInput */
    int64_t keyboard_type;   /* 4 = email */
    int64_t return_key_type; /* 2 = search */
    int64_t autocorrection;  /* 1 = no */
    int secure;
    char text[2048];
    size_t text_len;
    char marked[512];
    size_t marked_len;
    char call_log[MAX_CALL_LOG][32];
    char call_text[MAX_CALL_LOG][64];
    int call_count;
} fake_view_t;

typedef struct {
    int kind; /* KIND_ARRAY */
    void *items[16];
    size_t count;
} fake_array_t;

typedef struct {
    int kind; /* KIND_WINDOW */
    fake_array_t subviews;
    void *subview_items[16];
} fake_window_t;

typedef struct {
    int kind; /* KIND_APP */
    fake_window_t *key_window;
    fake_array_t windows;
    void *window_items[4];
} fake_app_t;

typedef struct {
    int kind; /* KIND_CLASS */
    const char *name;
} fake_class_t;

static fake_class_t g_cls_app = { KIND_CLASS, "UIApplication" };
static fake_class_t g_cls_responder = { KIND_CLASS, "UIResponder" };
static fake_class_t g_cls_nsstring = { KIND_CLASS, "NSString" };
static fake_class_t g_cls_notif = { KIND_CLASS, "NSNotificationCenter" };

static fake_app_t g_fake_app;
static fake_window_t g_fake_window;
static fake_view_t g_fake_textfield;
static fake_view_t g_fake_plainview;
static fake_obj_t g_fake_notif_center = { KIND_NOTIF_CENTER };
static fake_nsstring_t g_nsstring_pool[16];
static int g_nsstring_next;

static int g_sel_responds;
static int g_sel_shared_app;
static int g_sel_key_window;
static int g_sel_windows;
static int g_sel_count;
static int g_sel_object_at_index;
static int g_sel_subviews;
static int g_sel_is_first_responder;
static int g_sel_become;
static int g_sel_resign;
static int g_sel_has_text;
static int g_sel_insert_text;
static int g_sel_delete_backward;
static int g_sel_set_marked;
static int g_sel_unmark;
static int g_sel_secure;
static int g_sel_autocorrection;
static int g_sel_keyboard_type;
static int g_sel_return_key;
static int g_sel_string_utf8;
static int g_sel_default_center;
static int g_sel_post_notif;

static int g_note_will_show;
static int g_note_did_show;
static int g_note_will_hide;
static int g_note_did_hide;

static void *g_last_notif_name[MAX_NOTIF_LOG];
static void *g_last_notif_object[MAX_NOTIF_LOG];
static int g_notif_count;

static uint32_t g_last_kb_show;
static uint32_t g_last_kb_traits;
static float g_last_kb_frame_h;
static int g_kb_req_count;

static void *g_hook_become_imp;
static void *g_hook_resign_imp;
static void *g_orig_become_ret;
static void *g_orig_resign_ret;

static const char *g_env_keyboard;
static const char *g_env_trace;
static char g_log_buf[8192];
static size_t g_log_len;

static uint8_t fake_become_orig(void *self, void *cmd) {
    (void)self; (void)cmd;
    return 1;
}

static uint8_t fake_resign_orig(void *self, void *cmd) {
    (void)self; (void)cmd;
    return 1;
}

static void view_log(fake_view_t *v, const char *op, const char *text) {
    int i;
    if (v->call_count >= MAX_CALL_LOG) return;
    i = v->call_count++;
    strncpy(v->call_log[i], op, 31);
    v->call_log[i][31] = '\0';
    if (text) {
        strncpy(v->call_text[i], text, 63);
        v->call_text[i][63] = '\0';
    } else {
        v->call_text[i][0] = '\0';
    }
}

/* Remove the last UTF-8 scalar from v->text. */
static void view_pop_scalar(fake_view_t *v) {
    size_t n;
    if (v->text_len == 0) return;
    n = v->text_len;
    while (n > 1 && (v->text[n - 1] & 0xC0) == 0x80) n--;
    v->text_len = n - 1;
    v->text[v->text_len] = '\0';
}

static void *mock_objc_getClass(const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "UIApplication") == 0) return &g_cls_app;
    if (strcmp(name, "UIResponder") == 0) return &g_cls_responder;
    if (strcmp(name, "NSString") == 0) return &g_cls_nsstring;
    if (strcmp(name, "NSNotificationCenter") == 0) return &g_cls_notif;
    return NULL;
}

static void *mock_object_getClass(void *obj) {
    fake_obj_t *o = (fake_obj_t *)obj;
    if (!obj) return NULL;
    if (o->kind == KIND_APP) return &g_cls_app;
    if (o->kind == KIND_WINDOW) return &g_cls_app;
    return NULL;
}

static void *mock_class_getInstanceMethod(void *cls, void *sel) {
    if (cls == &g_cls_responder && sel == &g_sel_become) return (void *)(uintptr_t)0x9001;
    if (cls == &g_cls_responder && sel == &g_sel_resign) return (void *)(uintptr_t)0x9002;
    return NULL;
}

static void *mock_method_setImplementation(void *m, void *imp) {
    if (m == (void *)(uintptr_t)0x9001) {
        g_hook_become_imp = imp;
        return (void *)fake_become_orig;
    }
    if (m == (void *)(uintptr_t)0x9002) {
        g_hook_resign_imp = imp;
        return (void *)fake_resign_orig;
    }
    return NULL;
}

static void *mock_sel_registerName(const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "respondsToSelector:") == 0) return &g_sel_responds;
    if (strcmp(name, "sharedApplication") == 0) return &g_sel_shared_app;
    if (strcmp(name, "keyWindow") == 0) return &g_sel_key_window;
    if (strcmp(name, "windows") == 0) return &g_sel_windows;
    if (strcmp(name, "count") == 0) return &g_sel_count;
    if (strcmp(name, "objectAtIndex:") == 0) return &g_sel_object_at_index;
    if (strcmp(name, "subviews") == 0) return &g_sel_subviews;
    if (strcmp(name, "isFirstResponder") == 0) return &g_sel_is_first_responder;
    if (strcmp(name, "becomeFirstResponder") == 0) return &g_sel_become;
    if (strcmp(name, "resignFirstResponder") == 0) return &g_sel_resign;
    if (strcmp(name, "hasText") == 0) return &g_sel_has_text;
    if (strcmp(name, "insertText:") == 0) return &g_sel_insert_text;
    if (strcmp(name, "deleteBackward") == 0) return &g_sel_delete_backward;
    if (strcmp(name, "setMarkedText:selectedRange:") == 0) return &g_sel_set_marked;
    if (strcmp(name, "unmarkText") == 0) return &g_sel_unmark;
    if (strcmp(name, "isSecureTextEntry") == 0) return &g_sel_secure;
    if (strcmp(name, "autocorrectionType") == 0) return &g_sel_autocorrection;
    if (strcmp(name, "keyboardType") == 0) return &g_sel_keyboard_type;
    if (strcmp(name, "returnKeyType") == 0) return &g_sel_return_key;
    if (strcmp(name, "stringWithUTF8String:") == 0) return &g_sel_string_utf8;
    if (strcmp(name, "defaultCenter") == 0) return &g_sel_default_center;
    if (strcmp(name, "postNotificationName:object:userInfo:") == 0) return &g_sel_post_notif;
    return NULL;
}

static int view_responds(fake_view_t *v, void *sel) {
    if (sel == &g_sel_responds || sel == &g_sel_is_first_responder ||
        sel == &g_sel_become || sel == &g_sel_resign) return 1;
    if (v->kind != KIND_VIEW_TEXT) return 0;
    if (sel == &g_sel_has_text || sel == &g_sel_insert_text ||
        sel == &g_sel_delete_backward) return 1;
    if (sel == &g_sel_set_marked || sel == &g_sel_unmark) return v->supports_marked;
    if (sel == &g_sel_secure || sel == &g_sel_autocorrection ||
        sel == &g_sel_keyboard_type || sel == &g_sel_return_key) return 1;
    return 0;
}

/* Fake objc_msgSend. Third and fourth args carry the extra call
 * arguments: for setMarkedText:selectedRange: arg2 is the range
 * location and arg3 the range length (the struct is passed in two
 * registers on x86_64 and arm64). */
static void *mock_objc_msgSend(void *obj, void *sel, void *arg1, void *arg2, void *arg3) {
    fake_obj_t *o = (fake_obj_t *)obj;
    if (sel == &g_sel_responds) {
        if (!o) return NULL;
        if (o->kind == KIND_CLASS) {
            fake_class_t *c = (fake_class_t *)obj;
            if (strcmp(c->name, "UIApplication") == 0 && arg1 == &g_sel_shared_app) return (void *)(uintptr_t)1;
            if (strcmp(c->name, "NSString") == 0 && arg1 == &g_sel_string_utf8) return (void *)(uintptr_t)1;
            if (strcmp(c->name, "NSNotificationCenter") == 0 && arg1 == &g_sel_default_center) return (void *)(uintptr_t)1;
            return NULL;
        }
        if (o->kind == KIND_VIEW_TEXT || o->kind == KIND_VIEW_PLAIN) {
            return view_responds((fake_view_t *)obj, arg1) ? (void *)(uintptr_t)1 : NULL;
        }
        if (o->kind == KIND_APP) {
            if (arg1 == &g_sel_key_window || arg1 == &g_sel_windows) return (void *)(uintptr_t)1;
            return NULL;
        }
        if (o->kind == KIND_WINDOW) {
            if (arg1 == &g_sel_subviews || arg1 == &g_sel_is_first_responder) return (void *)(uintptr_t)1;
            return NULL;
        }
        if (o->kind == KIND_ARRAY) {
            if (arg1 == &g_sel_count || arg1 == &g_sel_object_at_index) return (void *)(uintptr_t)1;
            return NULL;
        }
        return NULL;
    }
    if (o && o->kind == KIND_CLASS) {
        fake_class_t *c = (fake_class_t *)obj;
        if (strcmp(c->name, "UIApplication") == 0 && sel == &g_sel_shared_app) return &g_fake_app;
        if (strcmp(c->name, "NSString") == 0 && sel == &g_sel_string_utf8) {
            fake_nsstring_t *s = &g_nsstring_pool[g_nsstring_next++ % 16];
            const char *t = (const char *)arg1;
            size_t n = t ? strlen(t) : 0;
            if (n >= sizeof(s->buf)) n = sizeof(s->buf) - 1;
            memcpy(s->buf, t, n);
            s->buf[n] = '\0';
            s->len = n;
            s->kind = KIND_NSSTRING;
            return s;
        }
        if (strcmp(c->name, "NSNotificationCenter") == 0 && sel == &g_sel_default_center) {
            return &g_fake_notif_center;
        }
        return NULL;
    }
    if (o && o->kind == KIND_NOTIF_CENTER && sel == &g_sel_post_notif) {
        if (g_notif_count < MAX_NOTIF_LOG) {
            g_last_notif_name[g_notif_count] = arg1;
            g_last_notif_object[g_notif_count] = arg2;
            g_notif_count++;
        }
        return NULL;
    }
    if (o && o->kind == KIND_APP) {
        if (sel == &g_sel_key_window) return g_fake_app.key_window;
        if (sel == &g_sel_windows) return &g_fake_app.windows;
        return NULL;
    }
    if (o && o->kind == KIND_WINDOW) {
        if (sel == &g_sel_subviews) return &g_fake_window.subviews;
        if (sel == &g_sel_is_first_responder) return NULL;
        return NULL;
    }
    if (o && o->kind == KIND_ARRAY) {
        fake_array_t *a = (fake_array_t *)obj;
        if (sel == &g_sel_count) return (void *)(uintptr_t)a->count;
        if (sel == &g_sel_object_at_index) {
            size_t i = (size_t)(uintptr_t)arg1;
            if (i < a->count) return a->items[i];
            return NULL;
        }
        return NULL;
    }
    if (o && (o->kind == KIND_VIEW_TEXT || o->kind == KIND_VIEW_PLAIN)) {
        fake_view_t *v = (fake_view_t *)obj;
        if (sel == &g_sel_is_first_responder) {
            return v->is_first_responder ? (void *)(uintptr_t)1 : NULL;
        }
        if (sel == &g_sel_has_text) {
            return v->text_len > 0 ? (void *)(uintptr_t)1 : NULL;
        }
        if (sel == &g_sel_insert_text) {
            fake_nsstring_t *s = (fake_nsstring_t *)arg1;
            view_log(v, "insertText:", s ? s->buf : "");
            if (s && v->text_len + s->len < sizeof(v->text)) {
                memcpy(v->text + v->text_len, s->buf, s->len);
                v->text_len += s->len;
                v->text[v->text_len] = '\0';
            }
            return NULL;
        }
        if (sel == &g_sel_delete_backward) {
            view_log(v, "deleteBackward", "");
            view_pop_scalar(v);
            return NULL;
        }
        if (sel == &g_sel_set_marked) {
            fake_nsstring_t *s = (fake_nsstring_t *)arg1;
            size_t loc = (size_t)(uintptr_t)arg2;
            size_t len = (size_t)(uintptr_t)arg3;
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "range={%zu,%zu}", loc, len);
            view_log(v, "setMarkedText:", tmp);
            if (s && s->len < sizeof(v->marked)) {
                memcpy(v->marked, s->buf, s->len);
                v->marked_len = s->len;
                v->marked[v->marked_len] = '\0';
            }
            return NULL;
        }
        if (sel == &g_sel_unmark) {
            view_log(v, "unmarkText", "");
            if (v->marked_len > 0 && v->text_len + v->marked_len < sizeof(v->text)) {
                memcpy(v->text + v->text_len, v->marked, v->marked_len);
                v->text_len += v->marked_len;
                v->text[v->text_len] = '\0';
            }
            v->marked_len = 0;
            v->marked[0] = '\0';
            return NULL;
        }
        if (sel == &g_sel_secure) return v->secure ? (void *)(uintptr_t)1 : NULL;
        if (sel == &g_sel_autocorrection) return (void *)(uintptr_t)v->autocorrection;
        if (sel == &g_sel_keyboard_type) return (void *)(uintptr_t)v->keyboard_type;
        if (sel == &g_sel_return_key) return (void *)(uintptr_t)v->return_key_type;
        if (sel == &g_sel_become) {
            v->is_first_responder = 1;
            return (void *)(uintptr_t)1;
        }
        if (sel == &g_sel_resign) {
            v->is_first_responder = 0;
            return (void *)(uintptr_t)1;
        }
        return NULL;
    }
    return NULL;
}

static void *mock_pool_push(void) { return (void *)(uintptr_t)0x1234; }
static void mock_pool_pop(void *p) { (void)p; }

static void *mock_dlsym(const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "objc_getClass") == 0) return (void *)mock_objc_getClass;
    if (strcmp(name, "object_getClass") == 0) return (void *)mock_object_getClass;
    if (strcmp(name, "class_getInstanceMethod") == 0) return (void *)mock_class_getInstanceMethod;
    if (strcmp(name, "method_setImplementation") == 0) return (void *)mock_method_setImplementation;
    if (strcmp(name, "sel_registerName") == 0) return (void *)mock_sel_registerName;
    if (strcmp(name, "objc_msgSend") == 0) return (void *)mock_objc_msgSend;
    if (strcmp(name, "objc_autoreleasePoolPush") == 0) return (void *)mock_pool_push;
    if (strcmp(name, "objc_autoreleasePoolPop") == 0) return (void *)mock_pool_pop;
    if (strcmp(name, "UIKeyboardWillShowNotification") == 0) return &g_note_will_show;
    if (strcmp(name, "UIKeyboardDidShowNotification") == 0) return &g_note_did_show;
    if (strcmp(name, "UIKeyboardWillHideNotification") == 0) return &g_note_will_hide;
    if (strcmp(name, "UIKeyboardDidHideNotification") == 0) return &g_note_did_hide;
    return NULL;
}

static const char *mock_getenv(const char *name) {
    if (strcmp(name, "IOSWINE_KEYBOARD") == 0) return g_env_keyboard;
    if (strcmp(name, "IOSWINE_KEYBOARD_TRACE") == 0) return g_env_trace;
    return NULL;
}

static void mock_log(const char *msg, size_t len) {
    size_t n = len;
    if (g_log_len + n >= sizeof(g_log_buf)) n = sizeof(g_log_buf) - 1 - g_log_len;
    memcpy(g_log_buf + g_log_len, msg, n);
    g_log_len += n;
    g_log_buf[g_log_len] = '\0';
}

static int mock_send_keyboard(uint32_t show, uint32_t traits, float frame_h_pts) {
    g_last_kb_show = show;
    g_last_kb_traits = traits;
    g_last_kb_frame_h = frame_h_pts;
    g_kb_req_count++;
    return 0;
}

static const keyboard_test_ops_t g_ops = {
    .dlsym_default = mock_dlsym,
    .getenv_fn = mock_getenv,
    .log_fn = mock_log,
    .send_keyboard_fn = mock_send_keyboard,
};

/* ------------------------------------------------------------------ */
/* Fixture                                                            */
/* ------------------------------------------------------------------ */

static void reset_fixture(void) {
    memset(&g_fake_app, 0, sizeof(g_fake_app));
    memset(&g_fake_window, 0, sizeof(g_fake_window));
    memset(&g_fake_textfield, 0, sizeof(g_fake_textfield));
    memset(&g_fake_plainview, 0, sizeof(g_fake_plainview));
    g_fake_app.kind = KIND_APP;
    g_fake_window.kind = KIND_WINDOW;
    g_fake_textfield.kind = KIND_VIEW_TEXT;
    g_fake_plainview.kind = KIND_VIEW_PLAIN;
    g_fake_textfield.supports_marked = 1;
    g_fake_textfield.keyboard_type = 4;   /* email */
    g_fake_textfield.return_key_type = 2; /* search */
    g_fake_textfield.autocorrection = 1;  /* no */
    g_fake_textfield.secure = 0;
    g_fake_app.key_window = &g_fake_window;
    g_fake_app.windows.kind = KIND_ARRAY;
    g_fake_app.windows.items[0] = &g_fake_window;
    g_fake_app.windows.count = 1;
    g_fake_window.subviews.kind = KIND_ARRAY;
    g_fake_window.subviews.items[0] = &g_fake_plainview;
    g_fake_window.subviews.items[1] = &g_fake_textfield;
    g_fake_window.subviews.count = 2;
    g_notif_count = 0;
    g_kb_req_count = 0;
    g_last_kb_show = 99;
    g_last_kb_traits = 0;
    g_last_kb_frame_h = -1.0f;
    g_nsstring_next = 0;
    g_hook_become_imp = NULL;
    g_hook_resign_imp = NULL;
    g_log_len = 0;
    g_log_buf[0] = '\0';
    g_env_keyboard = "1";
    g_env_trace = "1";
    keyboard_test_reset();
    keyboard_test_configure(&g_ops);
}

/* Call a recorded hook as the runtime would: (self, cmd). */
static uint8_t call_hook(void *imp, void *self) {
    uint8_t (*fn)(void *, void *) = (uint8_t (*)(void *, void *))imp;
    return fn(self, &g_sel_become);
}

/* ------------------------------------------------------------------ */
/* Tests                                                              */
/* ------------------------------------------------------------------ */

static uint32_t rng_state = 0x12345678u;
static uint32_t rng_next(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

int main(void) {
    /* 1. UTF-8 validation */
    CHECK(keyboard_validate_utf8("hello", 5) == 5);
    CHECK(keyboard_validate_utf8("caf\xc3\xa9", 5) == 4); /* e-acute, 2 bytes */
    CHECK(keyboard_validate_utf8("\xe2\x82\xac", 3) == 1); /* euro sign */
    CHECK(keyboard_validate_utf8("\xf0\x9f\x98\x80", 4) == 1); /* emoji */
    CHECK(keyboard_validate_utf8("", 0) == -1);
    CHECK(keyboard_validate_utf8(NULL, 3) == -1);
    CHECK(keyboard_validate_utf8("a\0b", 3) == -1);
    CHECK(keyboard_validate_utf8("\xc0\xaf", 2) == -1); /* overlong / */
    CHECK(keyboard_validate_utf8("\xed\xa0\x80", 3) == -1); /* surrogate */
    CHECK(keyboard_validate_utf8("\xe2\x82", 2) == -1); /* truncated */
    CHECK(keyboard_validate_utf8("\x80", 1) == -1); /* stray continuation */
    CHECK(keyboard_validate_utf8("\xf4\x90\x80\x80", 4) == -1); /* > U+10FFFF */
    CHECK(keyboard_validate_utf8("\xf4\x8f\xbf\xbf", 4) == 1); /* U+10FFFF ok */
    CHECK(keyboard_validate_utf8("a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80", 10) == 4);

    /* 2. Protocol round-trip: TEXT */
    {
        uint8_t buf[256];
        isws_text_t m, d;
        const char *s = "w\xc3\xb6rld";
        size_t n;
        m.kind = ISWS_TEXT_COMMIT;
        m.count = 6;
        m.utf8 = (const uint8_t *)s;
        m.utf8_len = 6;
        n = isws_encode_text(buf, sizeof(buf), &m);
        CHECK(n == 14);
        CHECK(isws_decode_text(buf, n, &d) == 0);
        CHECK(d.kind == ISWS_TEXT_COMMIT && d.count == 6 && d.utf8_len == 6);
        CHECK(memcmp(d.utf8, s, 6) == 0);
        m.kind = ISWS_TEXT_MARKED;
        n = isws_encode_text(buf, sizeof(buf), &m);
        CHECK(n == 14 && isws_decode_text(buf, n, &d) == 0 && d.kind == ISWS_TEXT_MARKED);
        m.kind = ISWS_TEXT_UNMARK;
        m.count = 0;
        m.utf8 = NULL;
        m.utf8_len = 0;
        n = isws_encode_text(buf, sizeof(buf), &m);
        CHECK(n == 8 && isws_decode_text(buf, n, &d) == 0 && d.kind == ISWS_TEXT_UNMARK);
        m.kind = ISWS_TEXT_DELETE;
        m.count = 3;
        n = isws_encode_text(buf, sizeof(buf), &m);
        CHECK(n == 8 && isws_decode_text(buf, n, &d) == 0 && d.kind == ISWS_TEXT_DELETE && d.count == 3);
        /* Rejections */
        m.kind = 99;
        CHECK(isws_encode_text(buf, sizeof(buf), &m) == 0);
        CHECK(isws_decode_text(buf, 4, &d) != 0); /* short payload */
        {
            uint8_t bad[12];
            isws_wr_u32le(bad + 0, 99);
            isws_wr_u32le(bad + 4, 0);
            CHECK(isws_decode_text(bad, 8, &d) != 0); /* bad kind */
            isws_wr_u32le(bad + 0, ISWS_TEXT_COMMIT);
            isws_wr_u32le(bad + 4, 99);
            CHECK(isws_decode_text(bad, 12, &d) != 0); /* count != length */
            isws_wr_u32le(bad + 0, ISWS_TEXT_DELETE);
            isws_wr_u32le(bad + 4, 1);
            CHECK(isws_decode_text(bad, 12, &d) != 0); /* delete with bytes */
            isws_wr_u32le(bad + 0, ISWS_TEXT_COMMIT);
            isws_wr_u32le(bad + 4, 4);
            CHECK(isws_decode_text(bad, 8, &d) != 0); /* commit, no bytes */
        }
        CHECK(isws_encode_text(buf, 4, &m) == 0); /* cap too small */
    }

    /* 2b. Protocol round-trip: KEYBOARD request */
    {
        uint8_t buf[64];
        isws_keyboard_req_t m, d;
        size_t n;
        m.show = 1;
        m.traits = KB_TRAIT_NO_AUTOCORRECT | (4u << KB_TRAIT_KB_TYPE_SHIFT) |
                   (2u << KB_TRAIT_RETURN_SHIFT);
        m.frame_h_pts = 0.0f;
        m.reserved = 0;
        n = isws_encode_keyboard_req(buf, sizeof(buf), &m);
        CHECK(n == ISWS_KEYBOARD_PAYLOAD_SIZE);
        CHECK(isws_decode_keyboard_req(buf, n, &d) == 0);
        CHECK(d.show == 1 && d.traits == m.traits && d.frame_h_pts == 0.0f);
        m.show = 2;
        CHECK(isws_encode_keyboard_req(buf, sizeof(buf), &m) == 0);
        CHECK(isws_decode_keyboard_req(buf, 8, &d) != 0);
    }

    /* 3. Init gating + hook install */
    reset_fixture();
    g_env_keyboard = NULL;
    keyboard_init();
    CHECK(g_hook_become_imp == NULL); /* env off: no hooks */
    reset_fixture();
    keyboard_init();
    CHECK(g_hook_become_imp != NULL);
    CHECK(g_hook_resign_imp != NULL);

    /* 4. become / resign hooks: show + hide, traits, notifications */
    reset_fixture();
    keyboard_init();
    CHECK(call_hook(g_hook_become_imp, &g_fake_textfield) == 1);
    CHECK(keyboard_test_first_responder() == &g_fake_textfield);
    CHECK(g_kb_req_count == 1 && g_last_kb_show == 1);
    CHECK((g_last_kb_traits & KB_TRAIT_NO_AUTOCORRECT) != 0);
    CHECK(((g_last_kb_traits >> KB_TRAIT_KB_TYPE_SHIFT) & 0xF) == 4);
    CHECK(((g_last_kb_traits >> KB_TRAIT_RETURN_SHIFT) & 0xF) == 2);
    CHECK(g_last_kb_frame_h == 0.0f);
    CHECK(g_notif_count == 2);
    CHECK(g_last_notif_name[0] == &g_note_will_show);
    CHECK(g_last_notif_name[1] == &g_note_did_show);
    CHECK(call_hook(g_hook_resign_imp, &g_fake_textfield) == 1);
    CHECK(keyboard_test_first_responder() == NULL);
    CHECK(g_kb_req_count == 2 && g_last_kb_show == 0);
    CHECK(g_notif_count == 4);
    CHECK(g_last_notif_name[2] == &g_note_will_hide);
    CHECK(g_last_notif_name[3] == &g_note_did_hide);

    /* 5. Commit ordering, multi-byte, emoji, invalid input, drops */
    reset_fixture();
    keyboard_init();
    keyboard_note_first_responder(&g_fake_textfield);
    CHECK(keyboard_commit_text("hello", 5) == 5);
    CHECK(keyboard_commit_text(" ", 1) == 1);
    CHECK(keyboard_commit_text("world", 5) == 5);
    CHECK(strcmp(g_fake_textfield.text, "hello world") == 0);
    CHECK(keyboard_commit_text("\xf0\x9f\x98\x80", 4) == 1);
    CHECK(strcmp(g_fake_textfield.text, "hello world\xf0\x9f\x98\x80") == 0);
    CHECK(keyboard_commit_text("\xc0\xaf", 2) == -1);
    CHECK(strcmp(g_fake_textfield.text, "hello world\xf0\x9f\x98\x80") == 0);
    CHECK(keyboard_commit_text(NULL, 3) == -1);
    CHECK(keyboard_commit_text("", 0) == -1);
    {
        uint64_t before = keyboard_dropped_count();
        keyboard_note_first_responder(NULL);
        CHECK(keyboard_commit_text("x", 1) == 0);
        CHECK(keyboard_dropped_count() == before + 1);
        keyboard_note_first_responder(&g_fake_plainview);
        CHECK(keyboard_commit_text("x", 1) == 0); /* plain view: not text */
        CHECK(keyboard_dropped_count() == before + 2);
    }

    /* 6. Marked text then commit; plain-UIKeyInput fallback */
    reset_fixture();
    keyboard_init();
    keyboard_note_first_responder(&g_fake_textfield);
    CHECK(keyboard_set_marked_text("ka", 2) == 2);
    CHECK(keyboard_test_marked_active() == 1);
    CHECK(strcmp(g_fake_textfield.marked, "ka") == 0);
    CHECK(g_fake_textfield.text_len == 0); /* marked is not committed yet */
    CHECK(strcmp(g_fake_textfield.call_log[0], "setMarkedText:") == 0);
    CHECK(keyboard_commit_text("kana", 4) == 4);
    CHECK(keyboard_test_marked_active() == 0);
    CHECK(strcmp(g_fake_textfield.text, "kakana") == 0); /* unmark commits "ka", then "kana" */
    CHECK(keyboard_unmark_text() == 0); /* nothing marked: no-op */
    /* Plain UIKeyInput only: marked falls back to direct commit. */
    g_fake_textfield.supports_marked = 0;
    g_fake_textfield.text_len = 0;
    g_fake_textfield.text[0] = '\0';
    CHECK(keyboard_set_marked_text("ab", 2) == 2);
    CHECK(strcmp(g_fake_textfield.text, "ab") == 0);
    CHECK(keyboard_test_marked_active() == 0);
    g_fake_textfield.supports_marked = 1;

    /* 7. deleteBackward */
    reset_fixture();
    keyboard_init();
    keyboard_note_first_responder(&g_fake_textfield);
    CHECK(keyboard_commit_text("hello", 5) == 5);
    CHECK(keyboard_delete_backward(2) == 2);
    CHECK(strcmp(g_fake_textfield.text, "hel") == 0);
    CHECK(keyboard_delete_backward(0) == 0);
    CHECK(keyboard_delete_backward(99) == 3); /* stops at empty via hasText */
    CHECK(strcmp(g_fake_textfield.text, "") == 0);
    CHECK(keyboard_delete_backward(1) == 0); /* empty buffer: no-op */
    CHECK(keyboard_delete_backward(1) == 0);
    /* Multi-byte delete removes one scalar, not one byte. */
    CHECK(keyboard_commit_text("a\xc3\xa9", 3) == 2);
    CHECK(keyboard_delete_backward(1) == 1);
    CHECK(strcmp(g_fake_textfield.text, "a") == 0);
    keyboard_note_first_responder(NULL);
    CHECK(keyboard_delete_backward(1) == 0);

    /* 8. Hardware keys */
    reset_fixture();
    keyboard_init();
    keyboard_note_first_responder(&g_fake_textfield);
    CHECK(keyboard_commit_text("abc", 3) == 3);
    {
        isws_key_t k;
        k.keycode = ISWS_KEY_DEL;
        k.action = ISWS_KEY_ACTION_DOWN;
        k.timestamp_ns = 0;
        CHECK(keyboard_handle_key(&k) == 0);
        CHECK(strcmp(g_fake_textfield.text, "ab") == 0);
        k.action = ISWS_KEY_ACTION_UP;
        CHECK(keyboard_handle_key(&k) == 1); /* key-up ignored */
        CHECK(strcmp(g_fake_textfield.text, "ab") == 0);
        k.keycode = ISWS_KEY_ENTER;
        k.action = ISWS_KEY_ACTION_DOWN;
        CHECK(keyboard_handle_key(&k) == 0);
        CHECK(strcmp(g_fake_textfield.text, "ab\n") == 0);
        k.keycode = ISWS_KEY_TAB;
        CHECK(keyboard_handle_key(&k) == 0);
        CHECK(strcmp(g_fake_textfield.text, "ab\n\t") == 0);
        k.keycode = ISWS_KEY_DPAD_LEFT;
        CHECK(keyboard_handle_key(&k) == 1); /* arrows: no cursor in v1 */
        k.keycode = 999;
        CHECK(keyboard_handle_key(&k) == 1); /* unknown keycode */
        CHECK(keyboard_handle_key(NULL) == -1);
        /* BACK hides the keyboard and resigns. */
        CHECK(g_kb_req_count == 1); /* show from note_first_responder */
        k.keycode = ISWS_KEY_BACK;
        CHECK(keyboard_handle_key(&k) == 0);
        CHECK(g_kb_req_count == 2 && g_last_kb_show == 0);
        CHECK(g_fake_textfield.is_first_responder == 0);
    }

    /* 9. First-responder discovery by walking the key window */
    reset_fixture();
    keyboard_init();
    g_fake_textfield.is_first_responder = 1;
    CHECK(keyboard_refresh_first_responder() == &g_fake_textfield);
    CHECK(keyboard_test_first_responder() == &g_fake_textfield);
    g_fake_textfield.is_first_responder = 0;
    g_fake_plainview.is_first_responder = 0;
    keyboard_test_reset();
    keyboard_test_configure(&g_ops);
    keyboard_init();
    CHECK(keyboard_refresh_first_responder() == NULL);
    g_fake_app.key_window = NULL;
    g_fake_textfield.is_first_responder = 1;
    CHECK(keyboard_refresh_first_responder() == &g_fake_textfield); /* falls back to windows[0] */

    /* 10. keyboard_handle_event dispatch */
    reset_fixture();
    keyboard_init();
    keyboard_note_first_responder(&g_fake_textfield);
    {
        present_event_t ev;
        const char *s = "hi";
        memset(&ev, 0, sizeof(ev));
        ev.kind = PRESENT_EV_TEXT;
        ev.u.text.kind = ISWS_TEXT_COMMIT;
        ev.u.text.count = 2;
        ev.u.text.utf8 = (const uint8_t *)s;
        ev.u.text.utf8_len = 2;
        CHECK(keyboard_handle_event(&ev) == 0);
        CHECK(strcmp(g_fake_textfield.text, "hi") == 0);
        ev.u.text.kind = ISWS_TEXT_MARKED;
        ev.u.text.count = 1;
        ev.u.text.utf8 = (const uint8_t *)"x";
        ev.u.text.utf8_len = 1;
        CHECK(keyboard_handle_event(&ev) == 0);
        CHECK(keyboard_test_marked_active() == 1);
        ev.u.text.kind = ISWS_TEXT_UNMARK;
        ev.u.text.count = 0;
        ev.u.text.utf8 = NULL;
        ev.u.text.utf8_len = 0;
        CHECK(keyboard_handle_event(&ev) == 0);
        CHECK(keyboard_test_marked_active() == 0);
        ev.u.text.kind = ISWS_TEXT_DELETE;
        ev.u.text.count = 1;
        CHECK(keyboard_handle_event(&ev) == 0);
        CHECK(strcmp(g_fake_textfield.text, "hi") == 0); /* unmark committed x, delete removed it */
        ev.u.text.kind = 99;
        CHECK(keyboard_handle_event(&ev) == 1);
        ev.kind = PRESENT_EV_KEY;
        ev.u.key.keycode = ISWS_KEY_DEL;
        ev.u.key.action = ISWS_KEY_ACTION_DOWN;
        ev.u.key.timestamp_ns = 0;
        CHECK(keyboard_handle_event(&ev) == 0);
        CHECK(strcmp(g_fake_textfield.text, "h") == 0);
        ev.kind = PRESENT_EV_FOCUS;
        CHECK(keyboard_handle_event(&ev) == 1);
        CHECK(keyboard_handle_event(NULL) == -1);
    }

    /* 11. Fuzz: random bytes through the validator; random valid
     * UTF-8 through commit. */
    {
        int i;
        int bad = 0, good = 0;
        char buf[64];
        size_t start_len;
        reset_fixture();
        keyboard_init();
        keyboard_note_first_responder(&g_fake_textfield);
        for (i = 0; i < 10000; i++) {
            size_t len = (size_t)(rng_next() % 32) + 1;
            size_t j;
            int r1, r2;
            for (j = 0; j < len; j++) buf[j] = (char)(rng_next() & 0xFF);
            r1 = keyboard_validate_utf8(buf, len);
            r2 = keyboard_validate_utf8(buf, len);
            CHECK(r1 == r2); /* deterministic */
            if (r1 < 0) bad++;
            else good++;
            CHECK(r1 <= (int)len);
        }
        CHECK(bad > 1000); /* random bytes are usually invalid */
        CHECK(good >= 0);
        start_len = g_fake_textfield.text_len;
        for (i = 0; i < 1000; i++) {
            /* Build a random valid UTF-8 string: ASCII + 2-byte chars. */
            char tmp[48];
            size_t pos = 0;
            int nch = (int)(rng_next() % 8) + 1;
            int c;
            for (c = 0; c < nch && pos + 2 < sizeof(tmp); c++) {
                if ((rng_next() & 1) != 0) {
                    tmp[pos++] = (char)('a' + (rng_next() % 26));
                } else {
                    tmp[pos++] = (char)0xC3;
                    tmp[pos++] = (char)(0xA0 + (rng_next() % 16));
                }
            }
            tmp[pos] = '\0';
            CHECK(keyboard_commit_text(tmp, pos) > 0);
        }
        CHECK(g_fake_textfield.text_len > start_len);
        CHECK(g_fake_textfield.text_len < sizeof(g_fake_textfield.text));
    }

    printf("keyboard_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
