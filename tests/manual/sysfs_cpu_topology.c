// /sys/devices/system/cpu: per-CPU topology and cache attributes.
//
// History: iSH published /sys/devices/system/cpu with the summary files
// (online/possible/present/kernel_max/offline) and a cpuN directory per CPU,
// but every cpuN directory was EMPTY -- no topology/, no cache/. lscpu reads
// those, so on-device it printed "Thread(s) per core: 0", "Core(s) per
// cluster: 0", "Cluster(s): 0" and "Socket(s): -", and hwloc/libgomp had no
// placement map at all. Two further gaps in the same code: sysfs readdir
// never emitted "." or ".." at any level (so `ls -a /sys/...` showed neither,
// unlike /proc), and stat() reported a size computed by a switch separate
// from the one producing the contents, so the two could drift.
//
// The topology published is deliberately FLAT -- one package, one cluster,
// every CPU its own core, no SMT -- even though the host is a big.LITTLE part
// whose performance/efficiency split iSH does know (the "host cores" line in
// /proc/cpuinfo). iSH's CPUs are host pthreads that the host scheduler
// migrates across clusters at will and sched_setaffinity is a no-op, so
// advertising clusters would invite hwloc and libgomp to make placement
// decisions that cannot take effect.
//
// Runs on real Linux too: the universal half asserts only things any kernel
// guarantees (masks and lists describing the same CPU set, self-membership,
// sane cache geometry, stat size matching read length, dot entries present).
// The flat-topology assertions are gated on running under iSH.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#include "test_common.h"

#define CPU_ROOT "/sys/devices/system/cpu"
#define MAX_CPUS 512

static int on_ish;

static void check(int cond, const char *what) {
    if (cond) {
        test_logf("ok: %s\n", what);
    } else {
        printf("FAIL: %s (errno=%d %s)\n", what, errno, strerror(errno));
        failures_total++;
    }
}

static void checkf(int cond, const char *fmt, ...) {
    if (cond)
        return;
    va_list ap;
    va_start(ap, fmt);
    printf("FAIL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    failures_total++;
}

// Read a whole sysfs attribute, stripping the trailing newline. Returns -1 if
// it cannot be opened (the caller decides whether that is a failure).
static int read_attr(const char *path, char *buf, size_t bufsize) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, bufsize - 1);
    close(fd);
    if (n < 0)
        return -1;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        n--;
    buf[n] = '\0';
    return 0;
}

static int read_attr_at(char *buf, size_t bufsize, const char *fmt, ...) {
    char path[PATH_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(path, sizeof(path), fmt, ap);
    va_end(ap);
    return read_attr(path, buf, bufsize);
}

// A CPU set, as both formats sysfs uses.
struct cpuset {
    unsigned char bits[MAX_CPUS];
    int count;
};

static void cpuset_add(struct cpuset *set, int cpu) {
    if (cpu < 0 || cpu >= MAX_CPUS)
        return;
    if (!set->bits[cpu])
        set->count++;
    set->bits[cpu] = 1;
}

// "0-3", "5", "0-1,4-7", "" (empty).
static int parse_list(const char *s, struct cpuset *out) {
    memset(out, 0, sizeof(*out));
    if (*s == '\0')
        return 0;
    while (*s != '\0') {
        char *end;
        long first = strtol(s, &end, 10);
        if (end == s)
            return -1;
        long last = first;
        s = end;
        if (*s == '-') {
            s++;
            last = strtol(s, &end, 10);
            if (end == s)
                return -1;
            s = end;
        }
        for (long c = first; c <= last; c++)
            cpuset_add(out, (int) c);
        if (*s == ',')
            s++;
        else if (*s != '\0')
            return -1;
    }
    return 0;
}

// "f", "ffffffff,0000000f" -- 32-bit groups, most significant first.
static int parse_mask(const char *s, struct cpuset *out) {
    memset(out, 0, sizeof(*out));
    // Count groups so we know each group's CPU offset.
    int groups = 1;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == ',')
            groups++;
    }
    int group = groups - 1;
    const char *p = s;
    while (1) {
        char word[16];
        size_t len = 0;
        while (*p != '\0' && *p != ',' && len < sizeof(word) - 1)
            word[len++] = *p++;
        word[len] = '\0';
        if (len == 0)
            return -1;
        unsigned long value = strtoul(word, NULL, 16);
        for (int bit = 0; bit < 32; bit++) {
            if (value & (1UL << bit))
                cpuset_add(out, group * 32 + bit);
        }
        if (*p != ',')
            break;
        p++;
        group--;
    }
    return 0;
}

static int cpuset_equal(const struct cpuset *a, const struct cpuset *b) {
    return memcmp(a->bits, b->bits, sizeof(a->bits)) == 0;
}

// Both formats of one attribute pair must describe the same set. This is the
// check that catches a marshalling bug in either direction.
static void check_mask_list_pair(int cpu, const char *mask_name, const char *list_name,
                                 struct cpuset *out) {
    char mask_text[512];
    char list_text[512];
    if (read_attr_at(mask_text, sizeof(mask_text), CPU_ROOT "/cpu%d/topology/%s", cpu, mask_name) != 0) {
        checkf(0, "cpu%d: read topology/%s", cpu, mask_name);
        memset(out, 0, sizeof(*out));
        return;
    }
    if (read_attr_at(list_text, sizeof(list_text), CPU_ROOT "/cpu%d/topology/%s", cpu, list_name) != 0) {
        checkf(0, "cpu%d: read topology/%s", cpu, list_name);
        memset(out, 0, sizeof(*out));
        return;
    }

    struct cpuset from_mask;
    struct cpuset from_list;
    checkf(parse_mask(mask_text, &from_mask) == 0, "cpu%d: parse %s \"%s\"", cpu, mask_name, mask_text);
    checkf(parse_list(list_text, &from_list) == 0, "cpu%d: parse %s \"%s\"", cpu, list_name, list_text);
    checkf(cpuset_equal(&from_mask, &from_list),
           "cpu%d: %s (\"%s\") and %s (\"%s\") describe the same CPU set",
           cpu, mask_name, mask_text, list_name, list_text);
    checkf(from_list.bits[cpu] != 0, "cpu%d: %s includes cpu%d itself", cpu, list_name, cpu);
    *out = from_list;
}

static int online_cpus(struct cpuset *out) {
    char text[512];
    if (read_attr(CPU_ROOT "/online", text, sizeof(text)) != 0) {
        check(0, "read " CPU_ROOT "/online");
        return -1;
    }
    if (parse_list(text, out) != 0) {
        checkf(0, "parse online list \"%s\"", text);
        return -1;
    }
    check(out->count > 0, "online list is non-empty");
    return 0;
}

// The regression that made `ls -a /sys/devices/system/cpu` show neither entry.
static void test_dot_entries(void) {
    static const char *dirs[] = {
        CPU_ROOT, CPU_ROOT "/cpu0", CPU_ROOT "/cpu0/topology", "/sys", "/sys/devices",
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        DIR *d = opendir(dirs[i]);
        if (d == NULL) {
            checkf(0, "opendir %s", dirs[i]);
            continue;
        }
        int saw_dot = 0;
        int saw_dotdot = 0;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0)
                saw_dot = 1;
            else if (strcmp(ent->d_name, "..") == 0)
                saw_dotdot = 1;
        }
        closedir(d);
        checkf(saw_dot, "%s: readdir emits \".\"", dirs[i]);
        checkf(saw_dotdot, "%s: readdir emits \"..\"", dirs[i]);
    }
}

// stat() size must equal what read() actually produces, or seek-based readers
// (and anything sizing a buffer from stat) get it wrong.
static void test_stat_size_matches(void) {
    static const char *files[] = {
        CPU_ROOT "/online",
        CPU_ROOT "/kernel_max",
        CPU_ROOT "/cpu0/topology/core_id",
        CPU_ROOT "/cpu0/topology/core_siblings",
        CPU_ROOT "/cpu0/topology/thread_siblings_list",
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        struct stat st;
        if (stat(files[i], &st) != 0) {
            checkf(0, "stat %s", files[i]);
            continue;
        }
        int fd = open(files[i], O_RDONLY);
        if (fd < 0) {
            checkf(0, "open %s", files[i]);
            continue;
        }
        char buf[512];
        ssize_t n = read(fd, buf, sizeof(buf));
        close(fd);
        checkf(n >= 0, "read %s", files[i]);
        // Real Linux reports a page-sized st_size for sysfs attributes; only
        // require that stat does not UNDER-report what read returns.
        checkf(st.st_size >= n, "%s: stat size %lld >= read length %lld",
               files[i], (long long) st.st_size, (long long) n);
        if (on_ish)
            checkf(st.st_size == n, "%s: stat size %lld == read length %lld (iSH reports exact)",
                   files[i], (long long) st.st_size, (long long) n);
    }
}

static void test_topology(const struct cpuset *online) {
    int highest = 0;
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        if (online->bits[cpu])
            highest = cpu;
    }

    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        if (!online->bits[cpu])
            continue;

        // Read both scalars first; if either is missing there is nothing
        // meaningful to compare, and parsing a stale buffer would report a
        // confusing value rather than the real problem.
        char text[512];
        int have_package = read_attr_at(text, sizeof(text),
                                        CPU_ROOT "/cpu%d/topology/physical_package_id", cpu) == 0;
        checkf(have_package,
               "cpu%d: topology/physical_package_id exists (was: no topology dir at all)", cpu);
        long package = have_package ? strtol(text, NULL, 10) : -1;

        int have_core_id = read_attr_at(text, sizeof(text),
                                        CPU_ROOT "/cpu%d/topology/core_id", cpu) == 0;
        checkf(have_core_id, "cpu%d: topology/core_id exists", cpu);
        if (!have_package || !have_core_id)
            continue;
        long core_id = strtol(text, NULL, 10);
        checkf(core_id >= 0, "cpu%d: core_id %ld is non-negative", cpu, core_id);

        struct cpuset threads;
        struct cpuset cores;
        check_mask_list_pair(cpu, "thread_siblings", "thread_siblings_list", &threads);
        check_mask_list_pair(cpu, "core_siblings", "core_siblings_list", &cores);
        checkf(threads.count <= cores.count,
               "cpu%d: thread siblings (%d) are a subset-sized set of core siblings (%d)",
               cpu, threads.count, cores.count);

        if (!on_ish)
            continue;

        // iSH's flat model: one package, one cluster, one thread per core.
        checkf(package == 0, "cpu%d: physical_package_id is 0, got %ld", cpu, package);
        checkf(core_id == cpu, "cpu%d: core_id equals the cpu number, got %ld", cpu, core_id);
        checkf(threads.count == 1, "cpu%d: exactly one thread sibling (no SMT), got %d",
               cpu, threads.count);
        checkf(cores.count == online->count,
               "cpu%d: core siblings span every online CPU (%d), got %d",
               cpu, online->count, cores.count);

        // package_cpus/cluster_cpus are the modern spellings lscpu prefers.
        struct cpuset package_cpus;
        struct cpuset cluster_cpus;
        check_mask_list_pair(cpu, "package_cpus", "package_cpus_list", &package_cpus);
        check_mask_list_pair(cpu, "cluster_cpus", "cluster_cpus_list", &cluster_cpus);
        checkf(cpuset_equal(&package_cpus, &cores),
               "cpu%d: package_cpus matches core_siblings", cpu);
        checkf(cpuset_equal(&cluster_cpus, &cores),
               "cpu%d: cluster_cpus matches core_siblings", cpu);
    }

    // A CPU past the end must not exist.
    char text[64];
    checkf(read_attr_at(text, sizeof(text), CPU_ROOT "/cpu%d/topology/core_id", highest + 1000) != 0,
           "cpu%d does not exist", highest + 1000);
}

static void test_cache(void) {
    int found = 0;
    for (int index = 0; index < 8; index++) {
        char level_text[64];
        if (read_attr_at(level_text, sizeof(level_text),
                         CPU_ROOT "/cpu0/cache/index%d/level", index) != 0)
            break;
        found++;

        long level = strtol(level_text, NULL, 10);
        checkf(level >= 1 && level <= 4, "cache index%d: level %ld in [1,4]", index, level);

        char type[64];
        checkf(read_attr_at(type, sizeof(type), CPU_ROOT "/cpu0/cache/index%d/type", index) == 0,
               "cache index%d: type exists", index);
        checkf(strcmp(type, "Data") == 0 || strcmp(type, "Instruction") == 0 ||
               strcmp(type, "Unified") == 0,
               "cache index%d: type \"%s\" is one of Data/Instruction/Unified", index, type);

        char size_text[64];
        checkf(read_attr_at(size_text, sizeof(size_text),
                            CPU_ROOT "/cpu0/cache/index%d/size", index) == 0,
               "cache index%d: size exists", index);
        char *suffix = NULL;
        long size_kb = strtol(size_text, &suffix, 10);
        checkf(size_kb > 0, "cache index%d: size %ld K is positive", index, size_kb);
        checkf(suffix != NULL && *suffix == 'K',
               "cache index%d: size \"%s\" carries Linux's K suffix", index, size_text);

        char line_text[64];
        checkf(read_attr_at(line_text, sizeof(line_text),
                            CPU_ROOT "/cpu0/cache/index%d/coherency_line_size", index) == 0,
               "cache index%d: coherency_line_size exists", index);
        long line = strtol(line_text, NULL, 10);
        checkf(line >= 16 && (line & (line - 1)) == 0,
               "cache index%d: coherency_line_size %ld is a sane power of two", index, line);

        // shared_cpu_map and shared_cpu_list must agree, and include cpu0.
        char map_text[512];
        char list_text[512];
        if (read_attr_at(map_text, sizeof(map_text),
                         CPU_ROOT "/cpu0/cache/index%d/shared_cpu_map", index) == 0 &&
            read_attr_at(list_text, sizeof(list_text),
                         CPU_ROOT "/cpu0/cache/index%d/shared_cpu_list", index) == 0) {
            struct cpuset from_map;
            struct cpuset from_list;
            checkf(parse_mask(map_text, &from_map) == 0,
                   "cache index%d: parse shared_cpu_map \"%s\"", index, map_text);
            checkf(parse_list(list_text, &from_list) == 0,
                   "cache index%d: parse shared_cpu_list \"%s\"", index, list_text);
            checkf(cpuset_equal(&from_map, &from_list),
                   "cache index%d: shared_cpu_map and shared_cpu_list agree", index);
            checkf(from_list.bits[0] != 0, "cache index%d: shared set includes cpu0", index);
        }
    }

    if (found == 0) {
        // Only tolerable where the host genuinely will not report geometry.
        printf("sysfs_cpu_topology: note: no cache/index* published\n");
        if (on_ish)
            printf("  (expected on a host that reports no cache geometry)\n");
    } else {
        test_logf("cache: %d index directories\n", found);
    }
}

// The per-CPU count in /proc/cpuinfo and the sysfs online list describe the
// same machine and must not disagree.
static void test_agrees_with_cpuinfo(const struct cpuset *online) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f == NULL) {
        check(0, "open /proc/cpuinfo");
        return;
    }
    int processors = 0;
    int harts = 0;
    char line[512];
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "processor", 9) == 0)
            processors++;
        else if (strncmp(line, "hart", 4) == 0)
            harts++;
    }
    fclose(f);
    // riscv64 emits BOTH a "processor" and a "hart" line per CPU -- that is the
    // real riscv kernel's format, and iSH matches it -- so counting either kind
    // as a CPU counts every riscv64 CPU twice. Count processors, falling back to
    // harts only for a kernel that omits the processor line entirely.
    if (processors == 0)
        processors = harts;
    checkf(processors == online->count,
           "/proc/cpuinfo lists %d CPUs and sysfs online lists %d",
           processors, online->count);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(30));

    struct utsname uts;
    if (uname(&uts) == 0 && strstr(uts.release, "ish") != NULL)
        on_ish = 1;
    test_logf("running on %s (iSH=%d)\n", uts.release, on_ish);

    struct stat st;
    if (stat(CPU_ROOT, &st) != 0) {
        printf("sysfs_cpu_topology: SKIP (%s not present)\n", CPU_ROOT);
        return 0;
    }

    struct cpuset online;
    if (online_cpus(&online) != 0)
        return finish_suite("sysfs_cpu_topology");
    test_logf("online CPUs: %d\n", online.count);

    test_agrees_with_cpuinfo(&online);
    test_dot_entries();
    test_stat_size_matches();
    test_topology(&online);
    test_cache();

    return finish_suite("sysfs_cpu_topology");
}
