/*
 * hostpresent_test.c - Host unit test for tools/hostpresent/hostpresent.c.
 *
 * Uses the real present.c client in PRESENT_TEST mode against the real
 * hostpresent server + control FIFO (compiled with -DHOSTPRESENT_LIB).
 * Covers:
 *   - HELLO -> READY + INFO (1440x3200 logical window)
 *   - FRAME -> FRAME_DONE (buffer 0 full frame + buffer 1 damage)
 *   - ctl FIFO `shot` -> valid RGBA PNG with expected pixel values
 *   - ctl FIFO `tap X Y` -> TOUCH DOWN + UP in client pixel coordinates
 *   - ctl FIFO `swipe X1 Y1 X2 Y2 MS` -> TOUCH DOWN + MOVEs + UP
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "hostpresent.h"
#include "present.h"
#include "protocol.h"

extern void present_test_set_socket_name(const char *name);

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void sleep_ms(int ms) {
    struct timespec ts = { .tv_sec = ms / 1000,
                           .tv_nsec = (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { }
}

static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t rd_le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int write_ctl(const char *ctl_path, const char *cmd) {
    int fd = open(ctl_path, O_WRONLY);
    if (fd < 0) return -1;
    size_t len = strlen(cmd);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, cmd + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        off += (size_t)n;
    }
    close(fd);
    return 0;
}

static int collect_events(present_event_t *out, int max_ev, int want_kind,
                          int min_count, int timeout_ms) {
    int count = 0;
    int64_t deadline = now_ms() + timeout_ms;
    while (count < max_ev) {
        present_event_t ev;
        while (count < max_ev && present_poll_event(&ev) == 1) {
            if (want_kind < 0 || (int)ev.kind == want_kind) {
                out[count++] = ev;
            }
        }
        if (count >= min_count) break;
        int64_t rem = deadline - now_ms();
        if (rem <= 0) break;
        int wait_slice = rem > 50 ? 50 : (int)rem;
        int wr = present_wait_readable(wait_slice);
        if (wr < 0) break;
        if (present_pump() != 0) break;
    }
    return count;
}

static void verify_png_file(const char *path, uint32_t exp_w, uint32_t exp_h) {
    FILE *fp = fopen(path, "rb");
    CHECK(fp != NULL);
    if (!fp) return;

    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    CHECK(fsz > 64);
    if (fsz <= 64) {
        fclose(fp);
        return;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)fsz);
    CHECK(buf != NULL);
    if (!buf) {
        fclose(fp);
        return;
    }
    size_t got = fread(buf, 1, (size_t)fsz, fp);
    fclose(fp);
    CHECK(got == (size_t)fsz);

    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    CHECK(memcmp(buf, sig, 8) == 0);

    size_t pos = 8;
    int saw_ihdr = 0, saw_idat = 0, saw_iend = 0;
    uint8_t *idat_data = NULL;
    size_t idat_len = 0;

    while (pos + 12 <= got) {
        uint32_t clen = rd_be32(buf + pos);
        const char *ctype = (const char *)(buf + pos + 4);
        CHECK(pos + 12 + (size_t)clen <= got);
        if (pos + 12 + (size_t)clen > got) break;
        const uint8_t *cdata = buf + pos + 8;

        if (memcmp(ctype, "IHDR", 4) == 0) {
            saw_ihdr = 1;
            CHECK(clen == 13);
            if (clen == 13) {
                CHECK(rd_be32(cdata + 0) == exp_w);
                CHECK(rd_be32(cdata + 4) == exp_h);
                CHECK(cdata[8] == 8);
                CHECK(cdata[9] == 6);
            }
        } else if (memcmp(ctype, "IDAT", 4) == 0) {
            saw_idat = 1;
            if (!idat_data) {
                idat_data = (uint8_t *)malloc(clen);
                if (idat_data) {
                    memcpy(idat_data, cdata, clen);
                    idat_len = clen;
                }
            }
        } else if (memcmp(ctype, "IEND", 4) == 0) {
            saw_iend = 1;
            CHECK(clen == 0);
        }
        pos += 12 + (size_t)clen;
    }
    CHECK(saw_ihdr);
    CHECK(saw_idat);
    CHECK(saw_iend);
    CHECK(pos == got);

    /* Unpack stored DEFLATE stream from IDAT and check first two pixels. */
    if (idat_data && idat_len >= 11) {
        CHECK(idat_data[0] == 0x78 && idat_data[1] == 0x01);
        /* First stored block header at offset 2..6. */
        uint16_t blen = rd_le16(idat_data + 3);
        uint16_t nlen = rd_le16(idat_data + 5);
        CHECK((uint16_t)(~blen) == nlen);
        CHECK(blen >= 9);
        /* Scanline 0 starts at idat_data + 7: filter=0, then RGBA pixels. */
        const uint8_t *row0 = idat_data + 7;
        CHECK(row0[0] == 0); /* filter byte 0 */
        /* Pixel (0,0) from BGRA 0xFF112233 -> RGBA (0x11, 0x22, 0x33, 0xFF). */
        CHECK(row0[1] == 0x11 && row0[2] == 0x22 &&
              row0[3] == 0x33 && row0[4] == 0xFF);
        /* Pixel (1,0) from BGRA 0x00000000 -> opaque white (0xFF,0xFF,0xFF,0xFF). */
        CHECK(row0[5] == 0xFF && row0[6] == 0xFF &&
              row0[7] == 0xFF && row0[8] == 0xFF);
    }

    free(idat_data);
    free(buf);
}

int main(void) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) tmp = "build/tmp";
    mkdir(tmp, 0777);

    char sock_name[64];
    char ctl_path[256];
    char out_path[256];
    snprintf(sock_name, sizeof(sock_name), "ioswine-hp-test-%d", (int)getpid());
    snprintf(ctl_path, sizeof(ctl_path), "%s/hp-test-%d.ctl", tmp, (int)getpid());
    snprintf(out_path, sizeof(out_path), "%s/hp-test-%d.png", tmp, (int)getpid());
    unlink(ctl_path);
    unlink(out_path);

    hostpresent_cfg_t cfg = {
        .socket_name = sock_name,
        .ctl_path = ctl_path,
        .out_path = out_path,
        .win_w = 0,
        .win_h = 0
    };
    hostpresent_ctx_t *hp = hostpresent_start(&cfg);
    CHECK(hp != NULL);
    if (!hp) return 1;

    present_test_set_socket_name(sock_name);

    const uint32_t W = 720u;
    const uint32_t H = 1600u;
    CHECK(present_init_timeout(W, H, 5000) == 0);
    CHECK(present_connected() == 1);

    /* Wait for INFO (1440x3200 logical window). */
    present_event_t info_ev[4];
    int n_info = collect_events(info_ev, 4, PRESENT_EV_INFO, 1, 2000);
    CHECK(n_info == 1);
    if (n_info == 1) {
        CHECK(info_ev[0].u.info.screen_w == 1440u);
        CHECK(info_ev[0].u.info.screen_h == 3200u);
        CHECK(info_ev[0].u.info.client_w == W);
        CHECK(info_ev[0].u.info.client_h == H);
        CHECK(fabsf(info_ev[0].u.info.scale - 2.0f) < 1e-4f);
        CHECK(fabsf(info_ev[0].u.info.offset_x) < 1e-4f);
        CHECK(fabsf(info_ev[0].u.info.offset_y) < 1e-4f);
        CHECK(info_ev[0].u.info.rotation == 0u);
    }

    /* Draw frame 0 (BGRA premul) and present -> wait for FRAME_DONE. */
    uint8_t *fb0 = present_framebuffer(0);
    CHECK(fb0 != NULL);
    if (fb0) {
        uint32_t *px = (uint32_t *)fb0;
        for (uint32_t i = 0; i < W * H; i++) {
            px[i] = 0xFF112233u;
        }
        px[1] = 0x00000000u; /* transparent pixel -> composited over white */
    }
    CHECK(present_present(0, NULL, 0) == 0);
    int64_t vsync = 0;
    CHECK(present_wait_vsync(&vsync, 5000) == 0);
    CHECK(vsync > 0);
    CHECK(present_last_frame_id() == 0);

    /* Present buffer 1 with a partial damage rect and verify FRAME_DONE id=1. */
    uint8_t *fb1 = present_framebuffer(1);
    CHECK(fb1 != NULL);
    if (fb1) {
        uint32_t *px = (uint32_t *)fb1;
        for (uint32_t i = 0; i < W * H; i++) {
            px[i] = 0xFF112233u;
        }
        px[1] = 0x00000000u;
    }
    isws_rect_t dmg = { .x = 10, .y = 10, .w = 20, .h = 20 };
    CHECK(present_present(1, &dmg, 1) == 0);
    CHECK(present_wait_vsync(&vsync, 5000) == 0);
    CHECK(present_last_frame_id() == 1);

    /* Request screenshot via ctl FIFO and verify PNG output. */
    CHECK(write_ctl(ctl_path, "shot\n") == 0);
    struct stat st;
    int got_png = 0;
    for (int i = 0; i < 40; i++) {
        if (stat(out_path, &st) == 0 && st.st_size > 0) {
            got_png = 1;
            break;
        }
        sleep_ms(25);
    }
    CHECK(got_png == 1);
    if (got_png) {
        verify_png_file(out_path, W, H);
    }

    /* Tap at (720, 1600) in 1440x3200 space -> client (360, 800). */
    CHECK(write_ctl(ctl_path, "tap 720 1600\n") == 0);
    present_event_t tap_evs[8];
    int n_tap = collect_events(tap_evs, 8, PRESENT_EV_TOUCH, 2, 2000);
    CHECK(n_tap == 2);
    if (n_tap == 2) {
        CHECK(tap_evs[0].u.touch.action == ISWS_TOUCH_DOWN);
        CHECK(tap_evs[0].u.touch.pointer_id == 0);
        CHECK(tap_evs[0].u.touch.pointer_count == 1);
        CHECK(fabsf(tap_evs[0].u.touch.x - 360.0f) < 0.05f);
        CHECK(fabsf(tap_evs[0].u.touch.y - 800.0f) < 0.05f);

        CHECK(tap_evs[1].u.touch.action == ISWS_TOUCH_UP);
        CHECK(tap_evs[1].u.touch.pointer_id == 0);
        CHECK(tap_evs[1].u.touch.pointer_count == 1);
        CHECK(fabsf(tap_evs[1].u.touch.x - 360.0f) < 0.05f);
        CHECK(fabsf(tap_evs[1].u.touch.y - 800.0f) < 0.05f);
        CHECK(tap_evs[1].u.touch.timestamp_ns >= tap_evs[0].u.touch.timestamp_ns);
    }

    /* Swipe (200, 1000) -> (1000, 1000) over 50 ms -> client (100,500)..(500,500).
     * 50 ms / 16 = 3 steps -> 1 DOWN + 3 MOVE + 1 UP = 5 events. */
    CHECK(write_ctl(ctl_path, "swipe 200 1000 1000 1000 50\n") == 0);
    present_event_t sw_evs[32];
    int n_sw = collect_events(sw_evs, 32, PRESENT_EV_TOUCH, 5, 2000);
    CHECK(n_sw >= 5);
    if (n_sw >= 5) {
        CHECK(sw_evs[0].u.touch.action == ISWS_TOUCH_DOWN);
        CHECK(fabsf(sw_evs[0].u.touch.x - 100.0f) < 0.05f);
        CHECK(fabsf(sw_evs[0].u.touch.y - 500.0f) < 0.05f);

        for (int i = 1; i < n_sw - 1; i++) {
            CHECK(sw_evs[i].u.touch.action == ISWS_TOUCH_MOVE);
            CHECK(sw_evs[i].u.touch.x >= sw_evs[i - 1].u.touch.x - 0.01f);
            CHECK(fabsf(sw_evs[i].u.touch.y - 500.0f) < 0.05f);
        }

        CHECK(sw_evs[n_sw - 1].u.touch.action == ISWS_TOUCH_UP);
        CHECK(fabsf(sw_evs[n_sw - 1].u.touch.x - 500.0f) < 0.05f);
        CHECK(fabsf(sw_evs[n_sw - 1].u.touch.y - 500.0f) < 0.05f);
    }

    present_shutdown();
    present_test_set_socket_name(NULL);
    hostpresent_stop(hp);
    unlink(out_path);
    unlink(ctl_path);

    /* Verify IOSWINE_SURFACE_SOCKET env var in present.c without test hook.
     * Also exercise custom vsync_hz (30 Hz) steady vsync timestamps. */
    char env_sock[64];
    char env_ctl[256];
    char env_out[256];
    snprintf(env_sock, sizeof(env_sock), "ioswine-hp-env-%d", (int)getpid());
    snprintf(env_ctl, sizeof(env_ctl), "%s/hp-env-%d.ctl", tmp, (int)getpid());
    snprintf(env_out, sizeof(env_out), "%s/hp-env-%d.png", tmp, (int)getpid());
    unlink(env_ctl);
    unlink(env_out);
    hostpresent_cfg_t cfg2 = {
        .socket_name = env_sock,
        .ctl_path = env_ctl,
        .out_path = env_out,
        .win_w = 0,
        .win_h = 0,
        .vsync_hz = 30
    };
    hostpresent_ctx_t *hp2 = hostpresent_start(&cfg2);
    CHECK(hp2 != NULL);
    if (hp2) {
        CHECK(setenv("IOSWINE_SURFACE_SOCKET", env_sock, 1) == 0);
        CHECK(present_init_timeout(W, H, 5000) == 0);
        CHECK(present_connected() == 1);
        int64_t v0 = 0, v1 = 0;
        CHECK(present_present(0, NULL, 0) == 0);
        CHECK(present_wait_vsync(&v0, 5000) == 0);
        sleep_ms(50);
        CHECK(present_present(1, NULL, 0) == 0);
        CHECK(present_wait_vsync(&v1, 5000) == 0);
        CHECK(v0 > 0);
        CHECK(v1 > v0);
        /* At 30 Hz (period 33333333 ns), a 50 ms wait spans >= 1 period. */
        CHECK((v1 - v0) >= 30000000LL);
        present_shutdown();
        unsetenv("IOSWINE_SURFACE_SOCKET");
        hostpresent_stop(hp2);
    }
    unlink(env_ctl);
    unlink(env_out);

    printf("hostpresent_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
