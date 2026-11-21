/* Network Extension Control Policy (NECP) stubs: unsupported on Linux.
 * Network.framework checks for < 0 / != 0 and falls back to non-NECP paths. */
#include "errno.h"
#include <stdint.h>
#include <stddef.h>

#define DARWIN_ENOTSUP 45

int lsk_necp_open(int flags) __asm__("_necp_open");
int lsk_necp_open(int flags) {
    (void)flags;
    set_errno(DARWIN_ENOTSUP);
    return -1;
}

int lsk_necp_client_action(int necp_fd, uint32_t action, const void *client_id,
                           size_t client_id_len, void *buffer, size_t buffer_size) __asm__("_necp_client_action");
int lsk_necp_client_action(int necp_fd, uint32_t action, const void *client_id,
                           size_t client_id_len, void *buffer, size_t buffer_size) {
    (void)necp_fd; (void)action; (void)client_id;
    (void)client_id_len; (void)buffer; (void)buffer_size;
    set_errno(DARWIN_ENOTSUP);
    return -1;
}

int lsk_necp_match_policy(const void *parameters, size_t parameters_size,
                          void *returned_result) __asm__("_necp_match_policy");
int lsk_necp_match_policy(const void *parameters, size_t parameters_size,
                          void *returned_result) {
    (void)parameters; (void)parameters_size; (void)returned_result;
    set_errno(DARWIN_ENOTSUP);
    return -1;
}

int lsk_necp_session_open(uint32_t flags) __asm__("_necp_session_open");
int lsk_necp_session_open(uint32_t flags) {
    (void)flags;
    set_errno(DARWIN_ENOTSUP);
    return -1;
}

int lsk_necp_session_action(int necp_fd, uint32_t action, void *in_buffer,
                            size_t in_buffer_len, void *out_buffer, size_t out_buffer_len) __asm__("_necp_session_action");
int lsk_necp_session_action(int necp_fd, uint32_t action, void *in_buffer,
                            size_t in_buffer_len, void *out_buffer, size_t out_buffer_len) {
    (void)necp_fd; (void)action; (void)in_buffer;
    (void)in_buffer_len; (void)out_buffer; (void)out_buffer_len;
    set_errno(DARWIN_ENOTSUP);
    return -1;
}
