/*
 * Minimal stub header for the harness. ipc.c includes "bks.h" for
 * bks_display_process_msg only. The real header
 * (libsystem_kernel/src/bks.h in the reference tree) also defines
 * the BKS MIG ids; this stub declares just what ipc.c needs so the
 * harness does not pull in any other file. The definition is a
 * host-only stub in stubs.c (returns 0 = not handled). See
 * README.md for what that means for kobject sends.
 *
 * No em dashes are used in this file.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ipc.h"

int bks_display_process_msg(const mach_msg_header_t *hdr, uint32_t send_size,
                            uint8_t *reply_buf, uint32_t max_reply_size,
                            uint32_t *out_reply_size);
