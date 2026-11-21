/*
 * sysinfo.c - Device status, storage, build info, and su-based launcher actions.
 */
#define _POSIX_C_SOURCE 200809L
#include "sysinfo.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

void isws_sysinfo_init(isws_sysinfo_t *info) {
    if (!info) return;
    memset(info, 0, sizeof(*info));
    snprintf(info->selinux, sizeof(info->selinux), "unknown");
    info->du_total_kb = -1;
    info->du_root_kb = -1;
    info->du_dsc_kb = -1;
    info->du_app_kb = -1;
    info->du_home_kb = -1;
    info->data_total_kb = -1;
    info->data_avail_kb = -1;
    info->last_exit_code = -1;
    info->last_duration_s = -1;
    info->last_frames = -1;
}

static void str_copy_trim(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n') src++;
    size_t n = strlen(src);
    while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == '\t' ||
                     src[n - 1] == '\r' || src[n - 1] == '\n')) {
        n--;
    }
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int64_t parse_i64_or_neg(const char *s) {
    if (!s) return -1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0' || (*s != '-' && !isdigit((unsigned char)*s))) return -1;
    char *end = NULL;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno != 0 || end == s) return -1;
    return (int64_t)v;
}

static void parse_file_line(isws_file_info_t *f, const char *val) {
    if (!f || !val) return;
    while (*val == ' ' || *val == '\t') val++;
    if (*val == '\0') return;
    char sha[65] = {0};
    long long sz = -1, mt = -1;
    int n = sscanf(val, "%lld %lld %64s", &sz, &mt, sha);
    if (n >= 2 && sz >= 0) {
        f->exists = true;
        f->size_bytes = (int64_t)sz;
        f->mtime_sec = (int64_t)mt;
        if (n == 3) {
            memcpy(f->sha256, sha, sizeof(f->sha256));
        }
    }
}

static void parse_df_line(isws_sysinfo_t *out, const char *line) {
    if (!out || !line) return;
    /* df -Pk /data last line: <fs> <1k-blocks> <used> <avail> <pct%> <mount> */
    char fs[128], pct[32], mnt[128];
    long long tot = -1, used = -1, avail = -1;
    if (sscanf(line, "%127s %lld %lld %lld %31s %127s",
               fs, &tot, &used, &avail, pct, mnt) >= 4) {
        if (tot >= 0) out->data_total_kb = (int64_t)tot;
        if (avail >= 0) out->data_avail_kb = (int64_t)avail;
    }
}

static void append_text_bounded(char *dst, size_t cap, const char *line) {
    if (!dst || cap == 0 || !line) return;
    size_t cur = strlen(dst);
    if (cur + 1 >= cap) return;
    size_t llen = strlen(line);
    if (cur > 0 && cur + 1 < cap) {
        dst[cur++] = '\n';
        dst[cur] = '\0';
    }
    size_t avail = cap - 1 - cur;
    if (llen > avail) llen = avail;
    memcpy(dst + cur, line, llen);
    dst[cur + llen] = '\0';
}

static bool contains_ci(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return false;
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) return true;
    }
    return false;
}

static bool is_crash_line(const char *line) {
    static const char *const k_pats[] = {
        "SIGSEGV", "SIGABRT", "SIGBUS", "SIGILL", "SIGSYS",
        "Segmentation fault", "FATAL", "panic", "Assertion",
        "unimplemented", "abort", "Killed", "Terminated"
    };
    if (!line || !*line) return false;
    /* Ignore our own launcher header/footer markers. */
    if (strncmp(line, "=== ioswine launch ", 19) == 0) return false;
    for (size_t i = 0; i < sizeof(k_pats) / sizeof(k_pats[0]); i++) {
        if (contains_ci(line, k_pats[i])) return true;
    }
    return false;
}

static void parse_meta_line(isws_sysinfo_t *out, const char *line) {
    if (!out || !line || !*line) return;
    const char *p;
    if (strncmp(line, "=== ioswine launch ", 19) == 0) {
        p = strstr(line, "exit=");
        if (p) {
            int ec = 0;
            if (sscanf(p, "exit=%d", &ec) == 1) {
                out->has_last_run = true;
                out->last_exit_code = ec;
            }
        }
        p = strstr(line, "duration=");
        if (p) {
            long long dur = 0;
            if (sscanf(p, "duration=%lld", &dur) == 1 && dur >= 0) {
                out->has_last_run = true;
                out->last_duration_s = (int64_t)dur;
            }
        }
        return;
    }
    if ((p = strstr(line, "presented=")) != NULL) {
        long long fr = -1;
        if (sscanf(p, "presented=%lld", &fr) == 1 && fr >= 0) {
            out->last_frames = (int64_t)fr;
        }
    } else if ((p = strstr(line, "frames=")) != NULL) {
        long long fr = -1;
        if (sscanf(p, "frames=%lld", &fr) == 1 && fr >= 0) {
            out->last_frames = (int64_t)fr;
        }
    }
    if (is_crash_line(line)) {
        str_copy_trim(out->last_crash, sizeof(out->last_crash), line);
        out->has_last_run = true;
    }
}

void isws_sysinfo_parse_probe(isws_sysinfo_t *out, const char *probe_output) {
    if (!out) return;
    isws_sysinfo_init(out);
    out->probed = true;
    if (!probe_output || !*probe_output) return;

    enum { SEC_KV = 0, SEC_REL, SEC_META, SEC_TAIL } sec = SEC_KV;
    const char *cur = probe_output;
    char line[1024];

    while (*cur) {
        const char *nl = strchr(cur, '\n');
        size_t len = nl ? (size_t)(nl - cur) : strlen(cur);
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, cur, len);
        line[len] = '\0';
        if (len > 0 && line[len - 1] == '\r') line[len - 1] = '\0';
        cur = nl ? (nl + 1) : (cur + strlen(cur));

        if (strcmp(line, "===RELEASE_BEGIN===") == 0) { sec = SEC_REL; continue; }
        if (strcmp(line, "===RELEASE_END===") == 0)   { sec = SEC_KV;  continue; }
        if (strcmp(line, "===LOG_META_BEGIN===") == 0){ sec = SEC_META;continue; }
        if (strcmp(line, "===LOG_META_END===") == 0)  { sec = SEC_KV;  continue; }
        if (strcmp(line, "===LOG_TAIL_BEGIN===") == 0){ sec = SEC_TAIL;continue; }
        if (strcmp(line, "===LOG_TAIL_END===") == 0)  { sec = SEC_KV;  continue; }

        if (sec == SEC_REL) {
            append_text_bounded(out->release_txt, sizeof(out->release_txt), line);
            continue;
        }
        if (sec == SEC_META) {
            parse_meta_line(out, line);
            continue;
        }
        if (sec == SEC_TAIL) {
            out->has_last_run = true;
            append_text_bounded(out->log_tail, sizeof(out->log_tail), line);
            if (is_crash_line(line) && out->last_crash[0] == '\0') {
                str_copy_trim(out->last_crash, sizeof(out->last_crash), line);
            }
            continue;
        }

        if (strncmp(line, "ROOT=1", 6) == 0) {
            out->root_available = true;
        } else if (strncmp(line, "SELINUX=", 8) == 0) {
            str_copy_trim(out->selinux, sizeof(out->selinux), line + 8);
            if (out->selinux[0] == '\0') snprintf(out->selinux, sizeof(out->selinux), "unknown");
        } else if (strncmp(line, "DU_TOTAL=", 9) == 0) {
            out->du_total_kb = parse_i64_or_neg(line + 9);
        } else if (strncmp(line, "DU_root=", 8) == 0) {
            out->du_root_kb = parse_i64_or_neg(line + 8);
        } else if (strncmp(line, "DU_dsc=", 7) == 0) {
            out->du_dsc_kb = parse_i64_or_neg(line + 7);
        } else if (strncmp(line, "DU_Aidoku.app=", 14) == 0) {
            out->du_app_kb = parse_i64_or_neg(line + 14);
        } else if (strncmp(line, "DU_home=", 8) == 0) {
            out->du_home_kb = parse_i64_or_neg(line + 8);
        } else if (strncmp(line, "DF_DATA=", 8) == 0) {
            parse_df_line(out, line + 8);
        } else if (strncmp(line, "FILE_ioswine-loader=", 20) == 0) {
            parse_file_line(&out->loader_info, line + 20);
        } else if (strncmp(line, "FILE_libsystem_kernel.dylib=", 28) == 0) {
            parse_file_line(&out->dylib_info, line + 28);
        } else if (strncmp(line, "PID=", 4) == 0) {
            int64_t pid = parse_i64_or_neg(line + 4);
            if (pid > 0) {
                out->loader_running = true;
                out->loader_pid = (int32_t)pid;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Process execution helpers                                           */
/* ------------------------------------------------------------------ */

static int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void exec_su(const char *cmd) {
    static const char *const k_su_paths[] = {
        "/system/bin/su",
        "/system/xbin/su",
        "/sbin/su",
        "/debug_ramdisk/su",
        "/su/bin/su",
        "/vendor/bin/su",
    };
    const char *argv[] = { "su", "-c", (char *)cmd, NULL };
    execvp("su", (char *const *)argv);
    for (size_t i = 0; i < sizeof(k_su_paths) / sizeof(k_su_paths[0]); i++) {
        execv(k_su_paths[i], (char *const *)argv);
    }
    _exit(127);
}

/* Run argv (or su -c cmd if use_su) capturing stdout+stderr into out_buf,
 * killing the child if timeout_ms > 0 expires. Returns child exit status
 * (>= 0) or -1 on spawn/timeout error. */
static int run_cmd_capture(bool use_su, const char *const argv[],
                           const char *su_cmd, int timeout_ms,
                           char *out_buf, size_t out_cap) {
    int pfd[2];
    if (out_buf && out_cap > 0) out_buf[0] = '\0';
    if (pipe(pfd) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            if (devnull > 2) close(devnull);
        }
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        if (use_su) {
            exec_su(su_cmd);
        } else {
            execvp(argv[0], (char *const *)argv);
        }
        _exit(127);
    }

    close(pfd[1]);
    int flags = fcntl(pfd[0], F_GETFL, 0);
    if (flags >= 0) fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK);

    size_t used = 0;
    int64_t deadline = timeout_ms > 0 ? mono_ms() + timeout_ms : 0;
    bool timed_out = false;
    bool eof = false;

    while (!eof) {
        int wait_ms = 200;
        if (timeout_ms > 0) {
            int64_t rem = deadline - mono_ms();
            if (rem <= 0) {
                timed_out = true;
                break;
            }
            wait_ms = (rem > 200) ? 200 : (int)rem;
        }
        struct pollfd p = { .fd = pfd[0], .events = POLLIN };
        int pr = poll(&p, 1, wait_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr > 0) {
            for (;;) {
                char tmp[512];
                ssize_t n = read(pfd[0], tmp, sizeof(tmp));
                if (n > 0) {
                    if (out_buf && out_cap > 1 && used + 1 < out_cap) {
                        size_t copy = (size_t)n;
                        if (copy > out_cap - 1 - used) copy = out_cap - 1 - used;
                        memcpy(out_buf + used, tmp, copy);
                        used += copy;
                        out_buf[used] = '\0';
                    }
                } else if (n == 0) {
                    eof = true;
                    break;
                } else {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    eof = true;
                    break;
                }
            }
        }
    }
    close(pfd[0]);

    if (timed_out) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }

    int st = 0;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return -1;
}

static const char *k_probe_cmd =
    "D=" ISWS_IOSWINE_DIR "; "
    "echo ROOT=1; "
    "echo SELINUX=$(getenforce 2>/dev/null || echo unknown); "
    "echo DU_TOTAL=$(du -sk \"$D\" 2>/dev/null | cut -f1); "
    "for s in root dsc Aidoku.app home; do "
    "  echo DU_${s}=$(du -sk \"$D/$s\" 2>/dev/null | cut -f1); "
    "done; "
    "echo DF_DATA=$(df -Pk /data 2>/dev/null | tail -n 1); "
    "for f in ioswine-loader libsystem_kernel.dylib; do "
    "  if [ -e \"$D/$f\" ]; then "
    "    echo FILE_${f}=$(stat -c '%s %Y' \"$D/$f\" 2>/dev/null) "
    "$(sha256sum \"$D/$f\" 2>/dev/null | cut -d' ' -f1); "
    "  fi; "
    "done; "
    "echo PID=$(pidof ioswine-loader 2>/dev/null || true); "
    "echo ===RELEASE_BEGIN===; "
    "cat \"$D/release.txt\" 2>/dev/null || true; "
    "echo ===RELEASE_END===; "
    "echo ===LOG_META_BEGIN===; "
    "grep -E '^=== ioswine launch ' \"$D/last-run.log\" 2>/dev/null | tail -n 4 || true; "
    "grep -iE 'SIGSEGV|SIGABRT|SIGBUS|SIGILL|SIGSYS|FATAL|panic|abort|Segmentation fault|unimplemented|Killed|Assertion' \"$D/last-run.log\" 2>/dev/null | tail -n 3 || true; "
    "grep -E 'presented=|frames=' \"$D/last-run.log\" 2>/dev/null | tail -n 3 || true; "
    "echo ===LOG_META_END===; "
    "echo ===LOG_TAIL_BEGIN===; "
    "tail -n 40 \"$D/last-run.log\" 2>/dev/null || true; "
    "echo ===LOG_TAIL_END===";

void isws_sysinfo_collect(isws_sysinfo_t *out) {
    if (!out) return;
    char buf[16384];
    int rc = run_cmd_capture(true, NULL, k_probe_cmd, 5000, buf, sizeof(buf));
    if (rc == 0 && strstr(buf, "ROOT=1") != NULL) {
        isws_sysinfo_parse_probe(out, buf);
        return;
    }

    /* Fallback without root: best-effort SELinux + /data statvfs. */
    isws_sysinfo_init(out);
    out->probed = true;
    out->root_available = false;

    const char *getenf_argv[] = { "getenforce", NULL };
    char se[64] = {0};
    if (run_cmd_capture(false, getenf_argv, NULL, 1000, se, sizeof(se)) == 0) {
        str_copy_trim(out->selinux, sizeof(out->selinux), se);
        if (out->selinux[0] == '\0') snprintf(out->selinux, sizeof(out->selinux), "unknown");
    } else {
        int fd = open("/sys/fs/selinux/enforce", O_RDONLY);
        if (fd >= 0) {
            char c = '\0';
            ssize_t r = read(fd, &c, 1);
            close(fd);
            if (r == 1) {
                snprintf(out->selinux, sizeof(out->selinux), "%s",
                         c == '1' ? "Enforcing" : "Permissive");
            }
        }
    }

    struct statvfs sv;
    if (statvfs("/data", &sv) == 0 ||
        statvfs("/data/data/org.ioswine.surface", &sv) == 0) {
        uint64_t fr = (uint64_t)(sv.f_frsize ? sv.f_frsize : sv.f_bsize);
        out->data_total_kb = (int64_t)(((uint64_t)sv.f_blocks * fr) / 1024u);
        out->data_avail_kb = (int64_t)(((uint64_t)sv.f_bavail * fr) / 1024u);
    }
}

int isws_launch_aidoku_blocking(char *err_buf, size_t err_cap) {
    char buf[1024];
    const char *cmd =
        "if [ ! -x " ISWS_LAUNCH_SCRIPT " ]; then "
        "echo 'missing " ISWS_LAUNCH_SCRIPT " (run release.sh)'; exit 126; "
        "fi; "
        "exec " ISWS_LAUNCH_SCRIPT;
    int rc = run_cmd_capture(true, NULL, cmd, 0, buf, sizeof(buf));
    if (err_buf && err_cap > 0) {
        str_copy_trim(err_buf, err_cap, buf);
    }
    return rc;
}

int isws_stop_aidoku(char *err_buf, size_t err_cap) {
    char buf[512];
    const char *cmd =
        "PIDS=$(pidof ioswine-loader 2>/dev/null || true); "
        "if [ -n \"$PIDS\" ]; then "
        "  kill -TERM $PIDS 2>/dev/null || true; "
        "  sleep 0.3; "
        "  PIDS2=$(pidof ioswine-loader 2>/dev/null || true); "
        "  [ -n \"$PIDS2\" ] && kill -KILL $PIDS2 2>/dev/null || true; "
        "fi; "
        "setenforce 1 2>/dev/null || true; "
        "getenforce 2>/dev/null || true";
    int rc = run_cmd_capture(true, NULL, cmd, 5000, buf, sizeof(buf));
    if (err_buf && err_cap > 0) {
        str_copy_trim(err_buf, err_cap, buf);
    }
    return (rc == 0) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Formatting helpers                                                  */
/* ------------------------------------------------------------------ */

void isws_fmt_kb(int64_t kb, char *buf, size_t cap) {
    if (!buf || cap == 0) return;
    if (kb < 0) {
        snprintf(buf, cap, "-");
        return;
    }
    double v = (double)kb;
    if (kb >= 1024LL * 1024LL) {
        snprintf(buf, cap, "%.2f GB", v / (1024.0 * 1024.0));
    } else if (kb >= 1024LL) {
        snprintf(buf, cap, "%.1f MB", v / 1024.0);
    } else {
        snprintf(buf, cap, "%lld KB", (long long)kb);
    }
}

void isws_fmt_bytes(int64_t bytes, char *buf, size_t cap) {
    if (!buf || cap == 0) return;
    if (bytes < 0) {
        snprintf(buf, cap, "-");
        return;
    }
    if (bytes >= 1024LL * 1024LL) {
        snprintf(buf, cap, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024LL) {
        snprintf(buf, cap, "%.1f KB", (double)bytes / 1024.0);
    } else {
        snprintf(buf, cap, "%lld B", (long long)bytes);
    }
}

void isws_fmt_epoch(int64_t epoch_sec, char *buf, size_t cap) {
    if (!buf || cap == 0) return;
    if (epoch_sec <= 0) {
        snprintf(buf, cap, "-");
        return;
    }
    time_t t = (time_t)epoch_sec;
    struct tm tm_v;
    if (!localtime_r(&t, &tm_v)) {
        snprintf(buf, cap, "%lld", (long long)epoch_sec);
        return;
    }
    snprintf(buf, cap, "%04d-%02d-%02d %02d:%02d",
             tm_v.tm_year + 1900, tm_v.tm_mon + 1, tm_v.tm_mday,
             tm_v.tm_hour, tm_v.tm_min);
}

void isws_fmt_duration(int64_t sec, char *buf, size_t cap) {
    if (!buf || cap == 0) return;
    if (sec < 0) {
        snprintf(buf, cap, "-");
        return;
    }
    if (sec < 60) {
        snprintf(buf, cap, "%llds", (long long)sec);
    } else if (sec < 3600) {
        snprintf(buf, cap, "%lldm %02llds", (long long)(sec / 60), (long long)(sec % 60));
    } else {
        snprintf(buf, cap, "%lldh %02lldm", (long long)(sec / 3600),
                 (long long)((sec % 3600) / 60));
    }
}
