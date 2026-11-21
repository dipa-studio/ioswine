/* Darwin commpage implementation at COMMPAGE_BASE (0xfffffc000, 16 KiB).
 * Freestanding: raw Linux syscalls only (sys.h, rt.h, no libc).
 * References:
 *   - XNU osfmk/arm/cpu_capabilities.h
 *   - Linux arm64 syscalls: sysinfo(179), sched_getaffinity(123), clock_gettime(113)
 */

#include <stdint.h>
#include <stddef.h>
#include "commpage.h"
#include "sys.h"
#include "rt.h"

/* Syscalls and clock IDs needed for commpage setup (not in sys.h) */
#define SYS_clock_gettime     113
#define SYS_sched_getaffinity 123
#define SYS_sysinfo           179

#define CLOCK_REALTIME 0
#define CLOCK_BOOTTIME 7

/* Linux arm64 HWCAP bits (from asm/hwcap.h) */
#define HWCAP_FP       (1ULL << 0)
#define HWCAP_ASIMD    (1ULL << 1)
#define HWCAP_AES      (1ULL << 3)
#define HWCAP_DIT      (1ULL << 24)

/* XNU commpage CPU capability flags (osfmk/arm/cpu_capabilities.h) */
#define _COMM_PAGE_CAP_ASIMD   (1ULL << 8)  /* kHasAdvSIMD (0x100) */
#define _COMM_PAGE_CAP_CRYPTO  (1ULL << 24) /* kHasARMv8Crypto (0x01000000) */
#define _COMM_PAGE_CAP_DIT     (1ULL << 57) /* kHasFeatDIT (bit 57) */

/* XNU commpage field offsets */
#define _COMM_PAGE_SIGNATURE             0x000 /* 16-byte signature string "commpage 64-bit" */
#define _COMM_PAGE_CPU_CAPABILITIES64    0x010 /* uint64_t CPU capabilities */
#define _COMM_PAGE_VERSION               0x01e /* uint16_t commpage format version (= 3) */
#define _COMM_PAGE_NCPUS                 0x022 /* uint8_t configured CPU count */
#define _COMM_PAGE_USER_PAGE_SHIFT_32    0x024 /* uint8_t 32-bit user page shift (= 14 for 16 KiB) */
#define _COMM_PAGE_USER_PAGE_SHIFT_64    0x025 /* uint8_t 64-bit user page shift (= 14 for 16 KiB) */
#define _COMM_PAGE_ACTIVE_CPUS           0x034 /* uint8_t active CPU count */
#define _COMM_PAGE_PHYSICAL_CPUS         0x035 /* uint8_t physical CPU count */
#define _COMM_PAGE_LOGICAL_CPUS          0x036 /* uint8_t logical CPU count */
#define _COMM_PAGE_KERNEL_PAGE_SHIFT     0x037 /* uint8_t kernel page shift (= 14 for 16 KiB) */
#define _COMM_PAGE_MEMORY_SIZE           0x038 /* uint64_t total physical memory bytes */
#define _COMM_PAGE_CPUFAMILY             0x080 /* uint32_t CPU family (= 0) */
#define _COMM_PAGE_TIMEBASE              0x088 /* uint64_t timebase delta (= 0) */
#define _COMM_PAGE_TIMEBASE_MODE         0x090 /* uint8_t timebase mode (= 1: cntvct_el0) */
#define _COMM_PAGE_CONT_TIMEBASE         0x098 /* uint64_t continuous timebase delta (= 0) */
#define _COMM_PAGE_BOOTTIME_USEC         0x0a0 /* uint64_t boot timestamp in microseconds */
#define _COMM_PAGE_APPROX_TIME           0x0c0 /* uint64_t approximate time (cntvct) */
#define _COMM_PAGE_KDEBUG_ENABLE         0x100 /* uint32_t kdebug enable flags (= 0) */
#define _COMM_PAGE_ATM_DIAGNOSTIC_CONFIG 0x104 /* uint32_t ATM diagnostic flags (= 0) */
#define _COMM_PAGE_OS_LOG_FLAGS          0x105 /* uint8_t os_log flags (= 0) */
#define _COMM_PAGE_MULTIUSER_CONFIG      0x108 /* uint32_t multiuser config (= 0) */
#define _COMM_PAGE_DYLD_FLAGS            0x10c /* uint8_t dyld fast-path flags (= 0) */
#define _COMM_PAGE_CPU_QUIESCENT_COUNTER 0x180 /* uint64_t generation counter (= 0) */

struct linux_timespec {
    long tv_sec;
    long tv_nsec;
};

struct linux_sysinfo {
    long uptime;
    unsigned long loads[3];
    unsigned long totalram;
    unsigned long freeram;
    unsigned long sharedram;
    unsigned long bufferram;
    unsigned long totalswap;
    unsigned long freeswap;
    uint16_t procs;
    uint16_t pad;
    unsigned long totalhigh;
    unsigned long freehigh;
    uint32_t mem_unit;
    char _f[20 - 2 * sizeof(unsigned long) - sizeof(uint32_t)];
};

static uint64_t g_commpage_hwcap = 0;
static uint64_t g_commpage_hwcap2 = 0;

/* External helper: allow loader to explicitly provide AT_HWCAP / AT_HWCAP2 */
void commpage_set_hwcap(uint64_t hwcap, uint64_t hwcap2) {
    g_commpage_hwcap = hwcap;
    g_commpage_hwcap2 = hwcap2;
}

/* Fallback: read AT_HWCAP and AT_HWCAP2 from /proc/self/auxv if not already set */
static void commpage_read_auxv_if_needed(void) {
    if (g_commpage_hwcap != 0 || g_commpage_hwcap2 != 0) return;
    long fd = sys4(SYS_openat, AT_FDCWD, (long)"/proc/self/auxv", O_RDONLY, 0);
    if (fd < 0) return;
    struct {
        uint64_t a_type;
        uint64_t a_val;
    } entry;
    while (sys3(SYS_read, fd, (long)&entry, sizeof(entry)) == (long)sizeof(entry)) {
        if (entry.a_type == 0) break;
        if (entry.a_type == 16) g_commpage_hwcap = entry.a_val;  /* AT_HWCAP */
        if (entry.a_type == 26) g_commpage_hwcap2 = entry.a_val; /* AT_HWCAP2 */
    }
    sys1(SYS_close, fd);
}

static uint64_t compute_cpu_capabilities(void) {
    commpage_read_auxv_if_needed();
    uint64_t caps = 0;
    if (g_commpage_hwcap & HWCAP_ASIMD) {
        caps |= _COMM_PAGE_CAP_ASIMD;
    }
    if (g_commpage_hwcap & HWCAP_AES) {
        caps |= _COMM_PAGE_CAP_CRYPTO;
    }
    if (g_commpage_hwcap & HWCAP_DIT) {
        caps |= _COMM_PAGE_CAP_DIT;
    }
    return caps;
}

/* Count available CPUs via sched_getaffinity (syscall 123) */
static uint8_t count_ncpus(void) {
    unsigned long mask[16];
    memset(mask, 0, sizeof(mask));
    long ret = sys3(SYS_sched_getaffinity, 0, sizeof(mask), (long)mask);
    if (ret <= 0) {
        return 1;
    }
    uint8_t count = 0;
    for (size_t i = 0; i < sizeof(mask) / sizeof(mask[0]); i++) {
        unsigned long w = mask[i];
        while (w) {
            count += (uint8_t)(w & 1);
            w >>= 1;
        }
    }
    return count > 0 ? count : 1;
}

/* Query total RAM bytes via sysinfo (syscall 179) */
static uint64_t get_memory_size(void) {
    struct linux_sysinfo sinfo;
    memset(&sinfo, 0, sizeof(sinfo));
    long ret = sys1(SYS_sysinfo, (long)&sinfo);
    if (ret < 0) {
        return 0;
    }
    uint64_t unit = sinfo.mem_unit ? sinfo.mem_unit : 1;
    return (uint64_t)sinfo.totalram * unit;
}

/* Compute boot time in microseconds since Unix epoch from CLOCK_REALTIME vs CLOCK_BOOTTIME */
static uint64_t get_boottime_usec(void) {
    struct linux_timespec real = {0, 0};
    struct linux_timespec boot = {0, 0};
    sys2(SYS_clock_gettime, CLOCK_REALTIME, (long)&real);
    sys2(SYS_clock_gettime, CLOCK_BOOTTIME, (long)&boot);

    long sec = real.tv_sec - boot.tv_sec;
    long nsec = real.tv_nsec - boot.tv_nsec;
    if (nsec < 0) {
        sec -= 1;
        nsec += 1000000000L;
    }
    return (uint64_t)sec * 1000000ULL + (uint64_t)(nsec / 1000L);
}

/* Read ARM64 virtual counter cntvct_el0 */
static inline uint64_t read_cntvct(void) {
    uint64_t val;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}

/* Map and fill the Darwin commpage at 0xfffffc000, then make it read-only.
 * Interface declared in commpage.h. Dies on error.
 */
void commpage_setup(void) {
    void *p = sys_mmap((void *)COMMPAGE_BASE, COMMPAGE_SIZE,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                       -1, 0);
    if (MAP_FAILED_P(p)) {
        die("commpage_setup: mmap(0x%lx, 0x%lx) failed", (uint64_t)COMMPAGE_BASE, (uint64_t)COMMPAGE_SIZE);
    }

    uint8_t *cp = (uint8_t *)p;
    memset(cp, 0, COMMPAGE_SIZE);

    /* 0x000: Signature string "commpage 64-bit" (XNU arm64 commpage) */
    memcpy(cp + _COMM_PAGE_SIGNATURE, "commpage 64-bit", 16);

    /* 0x010: CPU capabilities 64-bit (from host HWCAP / HWCAP2) */
    *(uint64_t *)(cp + _COMM_PAGE_CPU_CAPABILITIES64) = compute_cpu_capabilities();

    /* 0x01e: Commpage format version (= 3, XNU 12377 _COMM_PAGE_THIS_VERSION) */
    *(uint16_t *)(cp + _COMM_PAGE_VERSION) = 3;

    /* 0x022: NCPUS from sched_getaffinity */
    uint8_t ncpus = count_ncpus();
    *(uint8_t *)(cp + _COMM_PAGE_NCPUS) = ncpus;

    /* 0x024, 0x025: USER_PAGE_SHIFT_32 and USER_PAGE_SHIFT_64 = 14 (16 KiB iOS pages) */
    *(uint8_t *)(cp + _COMM_PAGE_USER_PAGE_SHIFT_32) = 14;
    *(uint8_t *)(cp + _COMM_PAGE_USER_PAGE_SHIFT_64) = 14;

    /* 0x034..0x036: ACTIVE_CPUS, PHYSICAL_CPUS, LOGICAL_CPUS */
    *(uint8_t *)(cp + _COMM_PAGE_ACTIVE_CPUS) = ncpus;
    *(uint8_t *)(cp + _COMM_PAGE_PHYSICAL_CPUS) = ncpus;
    *(uint8_t *)(cp + _COMM_PAGE_LOGICAL_CPUS) = ncpus;

    /* 0x037: KERNEL_PAGE_SHIFT = 14 */
    *(uint8_t *)(cp + _COMM_PAGE_KERNEL_PAGE_SHIFT) = 14;

    /* 0x038: MEMORY_SIZE from sysinfo */
    *(uint64_t *)(cp + _COMM_PAGE_MEMORY_SIZE) = get_memory_size();

    /* 0x080: CPUFAMILY = 0 */
    *(uint32_t *)(cp + _COMM_PAGE_CPUFAMILY) = 0;

    /* 0x088: TIMEBASE delta = 0 */
    *(uint64_t *)(cp + _COMM_PAGE_TIMEBASE) = 0;

    /* 0x090: TIMEBASE mode = 1 ("read cntvct_el0 directly") */
    *(uint8_t *)(cp + _COMM_PAGE_TIMEBASE_MODE) = 1;

    /* 0x098: CONT_TIMEBASE = 0 */
    *(uint64_t *)(cp + _COMM_PAGE_CONT_TIMEBASE) = 0;

    /* 0x0a0: BOOTTIME_USEC from CLOCK_BOOTTIME vs REALTIME */
    *(uint64_t *)(cp + _COMM_PAGE_BOOTTIME_USEC) = get_boottime_usec();

    /* 0x0c0: APPROX_TIME = current cntvct_el0 */
    *(uint64_t *)(cp + _COMM_PAGE_APPROX_TIME) = read_cntvct();

    /* 0x100: KDEBUG_ENABLE = 0 */
    *(uint32_t *)(cp + _COMM_PAGE_KDEBUG_ENABLE) = 0;

    /* 0x104: ATM_DIAGNOSTIC_CONFIG = 0 */
    *(uint32_t *)(cp + _COMM_PAGE_ATM_DIAGNOSTIC_CONFIG) = 0;

    /* 0x105: OS_LOG_FLAGS = 0 */
    *(uint8_t *)(cp + _COMM_PAGE_OS_LOG_FLAGS) = 0;

    /* 0x108: MULTIUSER_CONFIG = 0 */
    *(uint32_t *)(cp + _COMM_PAGE_MULTIUSER_CONFIG) = 0;

    /* 0x10c: dyld flags byte = 0 */
    *(uint8_t *)(cp + _COMM_PAGE_DYLD_FLAGS) = 0;

    /* 0x180: CPU_QUIESCENT_COUNTER = 0 */
    *(uint64_t *)(cp + _COMM_PAGE_CPU_QUIESCENT_COUNTER) = 0;

    /* Protect commpage as read-only */
    long ret = sys3(SYS_mprotect, (long)COMMPAGE_BASE, COMMPAGE_SIZE, PROT_READ);
    if (ret < 0) {
        die("commpage_setup: mprotect(PROT_READ) failed: %ld", ret);
    }
}
