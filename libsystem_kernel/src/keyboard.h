/*
 * keyboard.h - Android presenter IME to UIKit text input.
 *
 * Skeleton implementation. Activated only when
 * IOSWINE_KEYBOARD=1. Delivers presenter text (ISWS_MSG_TEXT) and
 * hardware keys (ISWS_MSG_KEY) to the current first responder via
 * the UIKeyInput / UITextInput protocols, and asks the presenter to
 * show or hide the Android soft keyboard (ISWS_MSG_KEYBOARD) when
 * a text field becomes or resigns first responder.
 *
 * Freestanding C11 for libsystem_kernel.dylib (arm64e, no libc, no
 * malloc, no __thread, no static function-pointer tables). ObjC/CF
 * are resolved via dlsym + lsk_sign_fn, like touch.c.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "protocol.h"
#include "present.h"

/* Text message kinds. */
#define KB_TEXT_COMMIT  0u /* committed text: -[UIKeyInput insertText:] */
#define KB_TEXT_MARKED  1u /* in-progress composition: -[UITextInput setMarkedText:selectedRange:] */
#define KB_TEXT_UNMARK  2u /* end composition: -[UITextInput unmarkText] */
#define KB_TEXT_DELETE  3u /* delete N characters: -[UIKeyInput deleteBackward:] x N */

/* Keyboard request traits (ISWS_MSG_KEYBOARD payload, bits 8..15 =
 * keyboard type, bits 16..23 = return key type). */
#define KB_TRAIT_SECURE         0x00000001u
#define KB_TRAIT_NO_AUTOCORRECT 0x00000002u
#define KB_TRAIT_KB_TYPE_SHIFT  8u
#define KB_TRAIT_RETURN_SHIFT   16u
#define KB_KB_TYPE_DEFAULT      0u
#define KB_KB_TYPE_ASCII        1u
#define KB_KB_TYPE_NUMBERS      2u
#define KB_KB_TYPE_PHONE        3u
#define KB_KB_TYPE_EMAIL        4u
#define KB_KB_TYPE_URL          5u
#define KB_RETURN_DEFAULT       0u
#define KB_RETURN_GO            1u
#define KB_RETURN_SEARCH        2u
#define KB_RETURN_SEND          3u
#define KB_RETURN_DONE          4u

/* Start the keyboard subsystem if IOSWINE_KEYBOARD=1; no-op otherwise.
 * Resolves ObjC/CF symbols and installs the UIResponder
 * becomeFirstResponder / resignFirstResponder tracking hooks. Safe
 * to call more than once. */
void keyboard_init(void);

/* Handle one presenter event dispatched by the touch reader thread
 * (touch.c owns the socket pump and calls
 * this for PRESENT_EV_TEXT and PRESENT_EV_KEY). Returns 0 if the
 * event was consumed, 1 if it was ignored (no first responder,
 * unknown keycode), -1 on a bad argument. Never blocks. */
int keyboard_handle_event(const present_event_t *ev);

/* Deliver committed UTF-8 text to the current first responder.
 * Returns the number of Unicode scalar values delivered, 0 if there
 * is no text first responder (text dropped), -1 on invalid input
 * (NULL, empty, invalid UTF-8, embedded NUL). */
int keyboard_commit_text(const char *utf8, size_t len);

/* Set marked (composing) text. Falls back to keyboard_commit_text
 * when the responder implements UIKeyInput only. Returns scalars
 * set, 0 if no text responder, -1 on invalid input. */
int keyboard_set_marked_text(const char *utf8, size_t len);

/* End composition. No-op when nothing is marked. Returns 0, -1 on
 * missing symbols. */
int keyboard_unmark_text(void);

/* Delete up to count characters backwards. Stops early when the
 * responder reports hasText == false. Returns deletions performed,
 * 0 if there is no text responder or nothing to delete. */
int keyboard_delete_backward(uint32_t count);

/* Deliver one hardware key event (ISWS_MSG_KEY payload). Only
 * ACTION_DOWN acts; ACTION_UP is acknowledged and ignored. Returns
 * 0 consumed, 1 ignored. */
int keyboard_handle_key(const isws_key_t *k);

/* Record the current first responder (called by the touch path when
 * a tap lands on a text field, and by the becomeFirstResponder
 * hook). Passing NULL clears the record. */
void keyboard_note_first_responder(void *responder);

/* Re-discover the first responder from [UIApplication keyWindow]
 * (walk the view hierarchy for isFirstResponder). Returns the
 * responder found, or NULL. */
void *keyboard_refresh_first_responder(void);

/* Ask the presenter to show (show != 0) or hide (show == 0) the
 * soft keyboard, with the current responder's input traits.
 * Returns 0 if the request was sent, -1 if not (not initialised,
 * no sender). */
int keyboard_request_show(int show);

/* Diagnostics: 1 if a keyboard is currently requested visible. */
int keyboard_is_visible(void);

/* Number of text deliveries dropped because there was no text
 * first responder (diagnostic counter). */
uint64_t keyboard_dropped_count(void);

/* Validate UTF-8. Returns the number of Unicode scalar values, or
 * -1 if the input is not strict valid UTF-8 or contains U+0000.
 * Overlong forms, surrogates and values above U+10FFFF are invalid. */
int keyboard_validate_utf8(const char *utf8, size_t len);

#ifdef KEYBOARD_HOST_TEST
typedef struct {
    void *(*dlsym_default)(const char *name);
    const char *(*getenv_fn)(const char *name);
    void (*log_fn)(const char *msg, size_t len);
    int (*send_keyboard_fn)(uint32_t show, uint32_t traits, float frame_h_pts);
} keyboard_test_ops_t;

void keyboard_test_configure(const keyboard_test_ops_t *ops);
void keyboard_test_reset(void);
void *keyboard_test_first_responder(void);
int keyboard_test_marked_active(void);
#endif
