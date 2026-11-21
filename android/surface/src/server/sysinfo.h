/*
 * sysinfo.h - Device status, storage, installed build, and launcher actions
 *             for the ioswine-surface presenter.
 *
 * Pure C11 + POSIX; host-testable (probe parser + formatters need no Android).
 */
#ifndef IOSWINE_SYSINFO_H
#define IOSWINE_SYSINFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ISWS_IOSWINE_DIR "/data/local/tmp/ioswine"
#define ISWS_LAUNCH_SCRIPT "/data/local/tmp/ioswine/launch.sh"
#define ISWS_LAST_LOG "/data/local/tmp/ioswine/last-run.log"

typedef struct {
    bool exists;
    int64_t size_bytes;
    int64_t mtime_sec;
    char sha256[65];
} isws_file_info_t;

typedef struct {
    bool probed;
    bool root_available;
    char selinux[32];

    /* Storage in KiB (-1 if unknown / missing). */
    int64_t du_total_kb;
    int64_t du_root_kb;
    int64_t du_dsc_kb;
    int64_t du_app_kb;
    int64_t du_home_kb;
    int64_t data_total_kb;
    int64_t data_avail_kb;

    /* Installed artifacts in /data/local/tmp/ioswine. */
    isws_file_info_t loader_info;
    isws_file_info_t dylib_info;
    char release_txt[512];

    /* Loader runtime state. */
    bool loader_running;
    int32_t loader_pid;

    /* Last run parsed from last-run.log (plus presenter session fallback). */
    bool has_last_run;
    int32_t last_exit_code;  /* -1 if unknown */
    int64_t last_duration_s; /* -1 if unknown */
    int64_t last_frames;     /* -1 if unknown from log */
    char last_crash[160];
    char log_tail[4096];
} isws_sysinfo_t;

/* Reset struct to unknown defaults. */
void isws_sysinfo_init(isws_sysinfo_t *info);

/* Parse captured stdout of the probe shell command into *out. */
void isws_sysinfo_parse_probe(isws_sysinfo_t *out, const char *probe_output);

/* Collect device stats (uses su -c with a 5s timeout, falls back to direct
 * getenforce + statvfs if root is unavailable). */
void isws_sysinfo_collect(isws_sysinfo_t *out);

/* Run /data/local/tmp/ioswine/launch.sh via `su -c`. Blocks until the
 * launched script exits. Returns child exit code (0 on success, -1 on
 * spawn/su failure). */
int isws_launch_aidoku_blocking(char *err_buf, size_t err_cap);

/* Stop any running ioswine-loader and restore SELinux Enforcing via `su -c`.
 * Returns 0 on success, -1 on failure. */
int isws_stop_aidoku(char *err_buf, size_t err_cap);

/* Formatting helpers (always NUL-terminated). */
void isws_fmt_kb(int64_t kb, char *buf, size_t cap);
void isws_fmt_bytes(int64_t bytes, char *buf, size_t cap);
void isws_fmt_epoch(int64_t epoch_sec, char *buf, size_t cap);
void isws_fmt_duration(int64_t sec, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* IOSWINE_SYSINFO_H */
