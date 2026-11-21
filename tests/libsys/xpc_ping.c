#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

int printf(const char *fmt, ...);
void exit(int status);

typedef void *xpc_object_t;
typedef void *xpc_connection_t;
typedef void *dispatch_queue_t;
typedef void (^xpc_handler_t)(xpc_object_t);

xpc_connection_t xpc_connection_create_mach_service(const char *name, dispatch_queue_t targetq, uint64_t flags);
void xpc_connection_set_event_handler(xpc_connection_t connection, xpc_handler_t handler);
void xpc_connection_resume(xpc_connection_t connection);
xpc_object_t xpc_connection_send_message_with_reply_sync(xpc_connection_t connection, xpc_object_t message);
void xpc_connection_cancel(xpc_connection_t connection);

xpc_object_t xpc_dictionary_create(const char * const *keys, const xpc_object_t *values, size_t count);
void xpc_dictionary_set_int64(xpc_object_t xdict, const char *key, int64_t value);
void xpc_dictionary_set_string(xpc_object_t xdict, const char *key, const char *string);
int64_t xpc_dictionary_get_int64(xpc_object_t xdict, const char *key);
bool xpc_dictionary_get_bool(xpc_object_t xdict, const char *key);
const char *xpc_dictionary_get_string(xpc_object_t xdict, const char *key);
void xpc_release(xpc_object_t object);

int main(void) {
    xpc_connection_t conn = xpc_connection_create_mach_service("com.apple.ioswine.ping", NULL, 0);
    if (!conn) {
        printf("FAIL: xpc_connection_create_mach_service failed\n");
        exit(1);
    }

    xpc_connection_set_event_handler(conn, ^(xpc_object_t event) {
        (void)event;
    });
    xpc_connection_resume(conn);

    xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(msg, "message", "ping");
    xpc_dictionary_set_string(msg, "payload", "device");

    xpc_object_t reply = xpc_connection_send_message_with_reply_sync(conn, msg);
    if (!reply) {
        printf("FAIL: reply is NULL\n");
        exit(1);
    }

    int64_t err = xpc_dictionary_get_int64(reply, "error");
    bool pong = xpc_dictionary_get_bool(reply, "pong");

    if (err == 0 && pong) {
        printf("xpc ping ok\n");
    } else {
        printf("FAIL: err=%lld pong=%d\n", (long long)err, (int)pong);
        exit(1);
    }

    xpc_release(reply);
    xpc_release(msg);
    xpc_connection_cancel(conn);
    xpc_release(conn);
    return 0;
}
