/*
 * Host-test stub for <android/log.h>.
 *
 * server.c logs through __android_log_print only. The stub records
 * every line (tag + message) in a ring buffer the test can grep, and
 * also prints it to stderr so a failing test shows the device-style
 * log. Test-only: never compiled into the APK (the real NDK header
 * is used there).
 */
#ifndef TEST_STUB_ANDROID_LOG_H
#define TEST_STUB_ANDROID_LOG_H

#include <stdarg.h>

#define ANDROID_LOG_UNKNOWN 0
#define ANDROID_LOG_DEFAULT 1
#define ANDROID_LOG_VERBOSE 2
#define ANDROID_LOG_DEBUG 3
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_WARN 5
#define ANDROID_LOG_ERROR 6
#define ANDROID_LOG_FATAL 7
#define ANDROID_LOG_SILENT 8

int __android_log_print(int prio, const char *tag, const char *fmt, ...);

/* Test helpers (implemented in test_server_stubs.c). */
void test_log_reset(void);
int test_log_contains(const char *substr);

#endif
