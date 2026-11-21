#include "linux.h"
#include "errno.h"
#include <stdint.h>
#include <stddef.h>

#define KERN_SUCCESS 0

struct mach_timebase_info {
    uint32_t numer;
    uint32_t denom;
};
typedef struct mach_timebase_info *mach_timebase_info_t;

struct timeval {
    int64_t tv_sec;
    int32_t tv_usec;
};

struct timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

uint64_t lsk_mach_absolute_time(void) __asm__("_mach_absolute_time");
uint64_t lsk_mach_absolute_time(void) {
    uint64_t val;
    __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(val));
    return val;
}

uint64_t lsk_mach_continuous_time(void) __asm__("_mach_continuous_time");
uint64_t lsk_mach_continuous_time(void) {
    return lsk_mach_absolute_time();
}

uint64_t lsk_mach_approximate_time(void) __asm__("_mach_approximate_time");
uint64_t lsk_mach_approximate_time(void) {
    return lsk_mach_absolute_time();
}

uint64_t lsk_mach_continuous_approximate_time(void) __asm__("_mach_continuous_approximate_time");
uint64_t lsk_mach_continuous_approximate_time(void) {
    return lsk_mach_absolute_time();
}

uint64_t lsk_mach_boottime_usec(void) __asm__("_mach_boottime_usec");
uint64_t lsk_mach_boottime_usec(void) {
    uint64_t *p = (uint64_t *)0xfffffc0a0;
    return *p;
}

static uint64_t gcd(uint64_t a, uint64_t b) {
    while (b != 0) {
        uint64_t t = b;
        b = a % b;
        a = t;
    }
    return a;
}

int lsk_mach_timebase_info(mach_timebase_info_t info) __asm__("_mach_timebase_info");
int lsk_mach_timebase_info(mach_timebase_info_t info) {
    if (!info) return 4; /* KERN_INVALID_ARGUMENT */
    uint64_t frq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
    if (frq == 0) frq = 24000000; /* Fallback 24 MHz */
    uint64_t g = gcd(1000000000ULL, frq);
    info->numer = (uint32_t)(1000000000ULL / g);
    info->denom = (uint32_t)(frq / g);
    return KERN_SUCCESS;
}

int lsk_mach_timebase_info_trap(mach_timebase_info_t info) __asm__("_mach_timebase_info_trap");
int lsk_mach_timebase_info_trap(mach_timebase_info_t info) {
    return lsk_mach_timebase_info(info);
}

int lsk_mach_wait_until(uint64_t deadline) __asm__("_mach_wait_until");
int lsk_mach_wait_until(uint64_t deadline) {
    uint64_t now = lsk_mach_absolute_time();
    if (deadline > now) {
        struct mach_timebase_info tb;
        lsk_mach_timebase_info(&tb);
        uint64_t delta_ticks = deadline - now;
        uint64_t delta_ns = (delta_ticks * tb.numer) / tb.denom;
        struct {
            long tv_sec;
            long tv_nsec;
        } req, rem;
        req.tv_sec = (long)(delta_ns / 1000000000ULL);
        req.tv_nsec = (long)(delta_ns % 1000000000ULL);
        sys2(SYS_nanosleep, (long)&req, (long)&rem);
    }
    return KERN_SUCCESS;
}

int lsk_mach_get_times(uint64_t *absolute_time, uint64_t *continuous_time, struct timespec *tp) __asm__("_mach_get_times");
int lsk_mach_get_times(uint64_t *absolute_time, uint64_t *continuous_time, struct timespec *tp) {
    uint64_t now = lsk_mach_absolute_time();
    if (absolute_time) *absolute_time = now;
    if (continuous_time) *continuous_time = now;
    if (tp) {
        struct timeval tv;
        sys2(SYS_gettimeofday, (long)&tv, 0);
        tp->tv_sec = tv.tv_sec;
        tp->tv_nsec = (int64_t)tv.tv_usec * 1000;
    }
    return 0;
}

int lsk_gettimeofday(struct timeval *tv, void *tz) __asm__("_gettimeofday");
int lsk_gettimeofday(struct timeval *tv, void *tz) {
    long ret = sys2(SYS_gettimeofday, (long)tv, (long)tz);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

int lsk_gettimeofday_m(struct timeval *tv, void *tz) __asm__("___gettimeofday");
int lsk_gettimeofday_m(struct timeval *tv, void *tz) {
    return lsk_gettimeofday(tv, tz);
}

int lsk_commpage_gettimeofday(struct timeval *tv) __asm__("___commpage_gettimeofday");
int lsk_commpage_gettimeofday(struct timeval *tv) {
    return lsk_gettimeofday(tv, 0);
}

int lsk_clock_gettime(int clk_id, struct timespec *tp) __asm__("_clock_gettime");
int lsk_clock_gettime(int clk_id, struct timespec *tp) {
    long ret = sys2(SYS_clock_gettime, clk_id, (long)tp);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

uint64_t lsk_clock_gettime_nsec_np(int clk_id) __asm__("_clock_gettime_nsec_np");
uint64_t lsk_clock_gettime_nsec_np(int clk_id) {
    struct timespec ts;
    lsk_clock_gettime(clk_id, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

struct mach_timespec {
    uint32_t tv_sec;
    uint32_t tv_nsec;
};

int lsk_host_get_clock_service(uint32_t host, int clock_id, uint32_t *clock_serv) __asm__("_host_get_clock_service");
int lsk_host_get_clock_service(uint32_t host, int clock_id, uint32_t *clock_serv) {
    (void)host;
    (void)clock_id;
    if (clock_serv) *clock_serv = 0x403;
    return KERN_SUCCESS;
}

int lsk_clock_get_time(uint32_t clock_serv, struct mach_timespec *cur_time) __asm__("_clock_get_time");
int lsk_clock_get_time(uint32_t clock_serv, struct mach_timespec *cur_time) {
    (void)clock_serv;
    struct timespec ts;
    lsk_clock_gettime(0 /* CLOCK_REALTIME */, &ts);
    if (cur_time) {
        cur_time->tv_sec = (uint32_t)ts.tv_sec;
        cur_time->tv_nsec = (uint32_t)ts.tv_nsec;
    }
    return KERN_SUCCESS;
}

int lsk_clock_get_attributes(uint32_t clock_serv, int flavor, void *attr, uint32_t *cnt) __asm__("_clock_get_attributes");
int lsk_clock_get_attributes(uint32_t clock_serv, int flavor, void *attr, uint32_t *cnt) {
    (void)clock_serv;
    (void)flavor;
    (void)attr;
    (void)cnt;
    return KERN_SUCCESS;
}

int lsk_clock_sleep_trap(uint32_t clock_serv, int sleep_type, int sec, int nsec, struct mach_timespec *remain) __asm__("_clock_sleep_trap");
int lsk_clock_sleep_trap(uint32_t clock_serv, int sleep_type, int sec, int nsec, struct mach_timespec *remain) {
    (void)clock_serv;
    (void)sleep_type;
    (void)remain;
    struct {
        long tv_sec;
        long tv_nsec;
    } req, rem;
    req.tv_sec = sec;
    req.tv_nsec = nsec;
    sys2(SYS_nanosleep, (long)&req, (long)&rem);
    return KERN_SUCCESS;
}

int lsk_clock_sleep(uint32_t clock_serv, int sleep_type, int sec, int nsec, struct mach_timespec *remain) __asm__("_clock_sleep");
int lsk_clock_sleep(uint32_t clock_serv, int sleep_type, int sec, int nsec, struct mach_timespec *remain) {
    return lsk_clock_sleep_trap(clock_serv, sleep_type, sec, nsec, remain);
}

int lsk_nanosleep(const struct timespec *req, struct timespec *rem) __asm__("_nanosleep");
int lsk_nanosleep(const struct timespec *req, struct timespec *rem) {
    long ret = sys2(SYS_nanosleep, (long)req, (long)rem);
    if (ret < 0) {
        set_errno(linux_to_darwin_errno(ret));
        return -1;
    }
    return 0;
}

int lsk_nanosleep_nocancel(const struct timespec *req, struct timespec *rem) __asm__("_nanosleep$NOCANCEL");
int lsk_nanosleep_nocancel(const struct timespec *req, struct timespec *rem) {
    return lsk_nanosleep(req, rem);
}
