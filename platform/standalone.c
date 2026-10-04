#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <mach-o/dyld.h>
#endif

#include "fs/dev.h"
#include "fs/proc/ish.h"
#include "jit/jit.h"
#include "kernel/BatteryStatus.h"
#include "kernel/errno.h"
#include "kernel/hostinfo.h"

void ISHDiagnosticsRecordGuestFatalSync(const char *kind, const char *summary, const char *detail) {
    if (getenv("ISH_TRACE_GUEST_FATAL") == NULL)
        return;
    fprintf(stderr, "[guest-fatal] kind=%s\n", kind != NULL ? kind : "?");
    if (summary != NULL)
        fprintf(stderr, "[guest-fatal] %s\n", summary);
    if (detail != NULL)
        fprintf(stderr, "[guest-fatal] %s\n", detail);
}

bool amd64_jit_preference_get(void) {
    return amd64_jit_is_enabled();
}

void amd64_jit_preference_set(bool enabled) {
    amd64_jit_set_enabled(enabled);
}

static char *dup_or_unknown(const char *value) {
    if (value == NULL || value[0] == '\0')
        value = "unknown";
    return strdup(value);
}

time_t buildTimestamp(void) {
    // The binary's own timestamp, not __DATE__/__TIME__. The latter is *this
    // file's* compile time, and an incremental build that relinks without
    // recompiling standalone.c reports a stale one -- exactly the failure this
    // identifier exists to prevent. The executable's mtime moves on every
    // relink, so it always describes the binary you are actually running.
    char path[4096];
    const char *executable = NULL;
#if defined(__APPLE__)
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0)
        executable = path;
#else
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n > 0) {
        path[n] = '\0';
        executable = path;
    }
#endif
    struct stat st;
    if (executable != NULL && stat(executable, &st) == 0)
        return st.st_mtime;
    return 0;
}

char *copyBuildVersion(void) {
    time_t built = buildTimestamp();
    if (built != 0) {
        struct tm tm;
        char stamp[64];
        // UTC, not local time: the point of this stamp is comparing builds
        // across machines, and they do not agree on a timezone. Formatting
        // locally made two iPads running binaries a minute apart report stamps
        // eight hours apart.
        if (gmtime_r(&built, &tm) != NULL &&
                strftime(stamp, sizeof(stamp), "built %Y-%m-%d %H:%MZ", &tm) > 0)
            return strdup(stamp);
    }
    return strdup(__DATE__ " " __TIME__);
}

char *copyHostArchitecture(void) {
    struct utsname uts;
    if (uname(&uts) == 0)
        return dup_or_unknown(uts.machine);
    return dup_or_unknown(NULL);
}

char *copyHostMachineIdentifier(void) {
    struct utsname uts;
    if (uname(&uts) == 0)
        return dup_or_unknown(uts.nodename);
    return dup_or_unknown(NULL);
}

char *copyHostDeviceName(void) {
    struct utsname uts;
    if (uname(&uts) == 0)
        return dup_or_unknown(uts.sysname);
    return dup_or_unknown(NULL);
}

char *copyHostCoreTopology(void) {
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    char buf[32];
    if (cpus < 1)
        cpus = 1;
    snprintf(buf, sizeof(buf), "%ld", cpus);
    return strdup(buf);
}

void hostCacheGeometry(struct host_cache_geometry *out) {
    if (out == NULL)
        return;
    *out = (struct host_cache_geometry) {0};
#ifdef __APPLE__
    // Same conservative choice as the app build (kernel/hostinfo.m): the plain
    // hw.l1*/hw.l2cachesize names report the smaller tier on Apple Silicon.
    size_t size;
    size = sizeof(out->l1i_size);
    if (sysctlbyname("hw.l1icachesize", &out->l1i_size, &size, NULL, 0) != 0)
        out->l1i_size = 0;
    size = sizeof(out->l1d_size);
    if (sysctlbyname("hw.l1dcachesize", &out->l1d_size, &size, NULL, 0) != 0)
        out->l1d_size = 0;
    size = sizeof(out->l2_size);
    if (sysctlbyname("hw.l2cachesize", &out->l2_size, &size, NULL, 0) != 0)
        out->l2_size = 0;
    size = sizeof(out->line_size);
    if (sysctlbyname("hw.cachelinesize", &out->line_size, &size, NULL, 0) != 0)
        out->line_size = 0;
#endif
    // Non-Darwin CLI hosts leave every field 0; sysfs then omits the cache
    // attributes entirely rather than publishing made-up geometry.
}

char *printHostInfo(void) {
    char *arch = copyHostArchitecture();
    char *machine = copyHostMachineIdentifier();
    char *device = copyHostDeviceName();
    char *cores = copyHostCoreTopology();
    if (arch == NULL || machine == NULL || device == NULL || cores == NULL) {
        free(arch);
        free(machine);
        free(device);
        free(cores);
        return dup_or_unknown(NULL);
    }

    size_t len = snprintf(NULL, 0,
        "host arch: %s\nhost machine: %s\nhost device: %s\nhost cores: %s\n",
        arch, machine, device, cores);
    char *info = malloc(len + 1);
    if (info != NULL) {
        snprintf(info, len + 1,
            "host arch: %s\nhost machine: %s\nhost device: %s\nhost cores: %s\n",
            arch, machine, device, cores);
    }
    free(arch);
    free(machine);
    free(device);
    free(cores);
    return info != NULL ? info : dup_or_unknown(NULL);
}

char *printUIDevice(void) {
    return "standalone";
}

// The command-line twins of kernel/BatteryStatus.m; kernel/BatteryStatus.h says
// why each needs both. There is no battery or thermal source here, so both say
// UNKNOWN. That is the truth, and it already has a meaning a guest understands:
// an empty /sys/class/power_supply, as on a Linux machine without a battery, and
// "unknown" in /proc/ish/thermal_state.
void hostBatteryStatus(struct host_battery_status *out) {
    if (out == NULL)
        return;
    *out = (struct host_battery_status) {
        .state = HOST_BATTERY_UNKNOWN,
        .level = -1,
        .low_power_mode = -1,
    };
}

enum host_thermal_state hostThermalState(void) {
    return HOST_THERMAL_UNKNOWN;
}

// The zone name from a path to a zone file: whatever follows "zoneinfo/".
// macOS links /etc/localtime to /var/db/timezone/zoneinfo/<Name>; Linux to
// /usr/share/zoneinfo/<Name>, or ../usr/share/zoneinfo/<Name> as timedatectl
// writes it. The posix/ and right/ trees hold the same zones under the same
// names, so they are looked through.
static bool zone_name_from_path(const char *path, char *buf, size_t size) {
    const char *name = NULL;
    for (const char *at = strstr(path, "zoneinfo/"); at != NULL; at = strstr(at + 1, "zoneinfo/")) {
        if (at == path || at[-1] == '/')
            name = at + strlen("zoneinfo/");
    }
    if (name == NULL)
        return false;
    if (strncmp(name, "posix/", 6) == 0 || strncmp(name, "right/", 6) == 0)
        name += 6;
    size_t len = strlen(name);
    if (len == 0 || len >= size)
        return false;
    memcpy(buf, name, len + 1);
    return true;
}

// Where the host keeps its zone. The /etc/localtime link comes first because it
// is the setting itself: it is what libc reads, what timedatectl and Debian's
// tzdata write, and all macOS has. /etc/timezone is only a fallback, for a Linux
// host whose /etc/localtime is a copy rather than a link -- Debian 13 stopped
// creating that file and keeps it updated only where it already exists.
// The command-line build has no language list, only the locale it was started
// in: "zh_CN.UTF-8" becomes "zh-CN". C and POSIX name no language.
bool hostPreferredLanguages(char *buf, size_t size) {
    if (buf == NULL || size == 0)
        return false;
    buf[0] = '\0';
    const char *names[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
    const char *locale = NULL;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && locale == NULL; i++) {
        const char *value = getenv(names[i]);
        if (value != NULL && value[0] != '\0')
            locale = value;
    }
    if (locale == NULL || strncmp(locale, "C.", 2) == 0 || strcmp(locale, "C") == 0 ||
        strcmp(locale, "POSIX") == 0)
        return false;
    size_t len = strcspn(locale, ".@");
    if (len == 0 || len + 2 > size)
        return false;
    for (size_t i = 0; i < len; i++)
        buf[i] = locale[i] == '_' ? '-' : locale[i];
    buf[len] = '\n';
    buf[len + 1] = '\0';
    return true;
}

bool hostTimeZoneName(char *buf, size_t size) {
    if (buf == NULL || size == 0)
        return false;
    buf[0] = '\0';

    char target[4096];
    ssize_t n = readlink("/etc/localtime", target, sizeof(target) - 1);
    if (n > 0) {
        target[n] = '\0';
        if (zone_name_from_path(target, buf, size))
            return true;
    }

    FILE *file = fopen("/etc/timezone", "r");
    if (file != NULL) {
        char line[256];
        bool ok = fgets(line, sizeof(line), file) != NULL;
        fclose(file);
        if (ok) {
            line[strcspn(line, " \t\r\n")] = '\0';
            size_t len = strlen(line);
            if (len > 0 && len < size) {
                memcpy(buf, line, len + 1);
                return true;
            }
        }
    }
    buf[0] = '\0';
    return false;
}

void jit_install_thread_exception_handler(void) {
}

bool jit_host_fault_mach_active(void) {
    // Standalone CLI has no Mach exception handler; the POSIX SIGBUS handler in
    // jit.c translates truncated-mmap host faults into guest SIGBUS.
    return false;
}

struct rtc_time_ish {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
};

#define RTC_RD_TIME ((int) 0x80247009u)

static int rtc_open(int major, int minor, struct fd *fd) {
    (void) major;
    (void) minor;
    (void) fd;
    return 0;
}

static ssize_t rtc_read(struct fd *fd, void *buf, size_t bufsize) {
    (void) fd;
    (void) buf;
    (void) bufsize;
    return 0;
}

static int rtc_poll(struct fd *fd) {
    (void) fd;
    return 0;
}

static int rtc_close(struct fd *fd) {
    (void) fd;
    return 0;
}

static ssize_t rtc_ioctl_size(int cmd) {
    if (cmd == RTC_RD_TIME)
        return sizeof(struct rtc_time_ish);
    return -1;
}

static int rtc_ioctl(struct fd *fd, int cmd, void *arg) {
    (void) fd;
    if (cmd != RTC_RD_TIME || arg == NULL)
        return _EINVAL;

    time_t now = time(NULL);
    struct tm tm_now;
    if (localtime_r(&now, &tm_now) == NULL)
        return errno_map();

    struct rtc_time_ish *rtc = arg;
    rtc->tm_sec = tm_now.tm_sec;
    rtc->tm_min = tm_now.tm_min;
    rtc->tm_hour = tm_now.tm_hour;
    rtc->tm_mday = tm_now.tm_mday;
    rtc->tm_mon = tm_now.tm_mon;
    rtc->tm_year = tm_now.tm_year;
    return 0;
}

struct dev_ops rtc_dev = {
    .open = rtc_open,
    .fd.read = rtc_read,
    .fd.poll = rtc_poll,
    .fd.close = rtc_close,
    .fd.ioctl = rtc_ioctl,
    .fd.ioctl_size = rtc_ioctl_size,
};
