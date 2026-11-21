/*
 * necp_test.c -- host test for NECP syscalls in libsystem_kernel/src/necp.c.
 */
#include <stdint.h>
#include <stdio.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define DARWIN_ENOTSUP 45

static int g_darwin_errno = 0;
void set_errno(int err) { g_darwin_errno = err; }

extern int lsk_necp_open(int flags) __asm__("_necp_open");
extern int lsk_necp_client_action(int necp_fd, uint32_t action, const void *client_id,
                                  size_t client_id_len, void *buffer, size_t buffer_size) __asm__("_necp_client_action");
extern int lsk_necp_match_policy(const void *parameters, size_t parameters_size,
                                 void *returned_result) __asm__("_necp_match_policy");
extern int lsk_necp_session_open(uint32_t flags) __asm__("_necp_session_open");
extern int lsk_necp_session_action(int necp_fd, uint32_t action, void *in_buffer,
                                   size_t in_buffer_len, void *out_buffer, size_t out_buffer_len) __asm__("_necp_session_action");

int main(void) {
    uint8_t buf[32] = {0};

    g_darwin_errno = 0;
    CHECK(lsk_necp_open(0) == -1);
    CHECK(g_darwin_errno == DARWIN_ENOTSUP);

    g_darwin_errno = 0;
    CHECK(lsk_necp_open(1) == -1);
    CHECK(g_darwin_errno == DARWIN_ENOTSUP);

    g_darwin_errno = 0;
    CHECK(lsk_necp_client_action(-1, 27, buf, 16, buf, sizeof(buf)) == -1);
    CHECK(g_darwin_errno == DARWIN_ENOTSUP);

    g_darwin_errno = 0;
    CHECK(lsk_necp_match_policy(buf, sizeof(buf), buf) == -1);
    CHECK(g_darwin_errno == DARWIN_ENOTSUP);

    g_darwin_errno = 0;
    CHECK(lsk_necp_session_open(0) == -1);
    CHECK(g_darwin_errno == DARWIN_ENOTSUP);

    g_darwin_errno = 0;
    CHECK(lsk_necp_session_action(-1, 1, buf, sizeof(buf), buf, sizeof(buf)) == -1);
    CHECK(g_darwin_errno == DARWIN_ENOTSUP);

    printf("necp_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
