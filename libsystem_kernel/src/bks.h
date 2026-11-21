#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ipc.h"

#define BKS_DISPLAY_MIG_GET_MAIN_SCREEN_INFO 6001012
#define BKS_DISPLAY_MIG_REPLY_BASE           6001112

int bks_display_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                            uint8_t *reply_buf, uint32_t max_reply_size,
                            uint32_t *out_reply_size);
