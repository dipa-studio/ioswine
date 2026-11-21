/*
 * hostpresent.h - Host presenter stub for qemu / host tests.
 *
 * Plain Linux C interface. Standalone binary built without -DHOSTPRESENT_LIB;
 * host tests compile hostpresent.c with -DHOSTPRESENT_LIB to run the real
 * server and control FIFO in-process under ASan/UBSan.
 */
#pragma once

#include <stdint.h>

typedef struct {
    const char *socket_name; /* NULL -> ISWS_SOCKET_NAME ("ioswine-surface") */
    const char *ctl_path;    /* NULL -> "/data/local/tmp/ioswine/hostpresent.ctl" */
    const char *out_path;    /* NULL -> "/data/local/tmp/ioswine/host-frame.png" */
    uint32_t win_w;          /* 0 -> 1440 */
    uint32_t win_h;          /* 0 -> 3200 */
    uint32_t vsync_hz;       /* 0 -> 60 */
} hostpresent_cfg_t;

typedef struct hostpresent_ctx hostpresent_ctx_t;

/* Bind the abstract socket, create the control FIFO, and start background
 * threads. Returns NULL on setup failure. */
hostpresent_ctx_t *hostpresent_start(const hostpresent_cfg_t *cfg);

/* Stop background threads, close sockets/FDs, unlink the control FIFO, and
 * free ctx. Safe to pass NULL. */
void hostpresent_stop(hostpresent_ctx_t *ctx);
