// Placing a mapping must not cost more because something big is, or was,
// mapped.
//
// mmap without MAP_FIXED asks the kernel for a hole. AOK's hole finder used to
// find the edges of each occupied region by looking at the page-table entry of
// every page in it -- 56 bytes a page -- so a single 2 GiB mapping made every
// later mmap(NULL, ...) touch ~28 MB. Measured on an amd64 guest, 1000 x
// mmap(NULL, 4096) cost:
//
//   nothing else mapped                             0.02 ms each
//   256 MiB PROT_NONE MAP_SHARED of a memfd          0.37 ms each
//   2 GiB   PROT_NONE MAP_SHARED of a memfd          11-19 ms each
//
// (A 2 GiB MAP_PRIVATE|MAP_ANONYMOUS reservation stayed at 0.02: large
// anonymous mappings are lazy and have no entries to walk. File and memfd
// mappings always have them.) .NET's W^X double mapper maps its code heap as a
// MAP_SHARED memfd, and `dotnet --info` spent most of an hour in this walk.
// Linux answers the same question from a VMA tree in microseconds.
//
// And the cost OUTLIVED the mapping. Page-table leaves are never freed while
// the address space lives, and the search for the next mapped page read every
// entry of every leaf it found, empty or not -- so once a process had mapped
// 2 GiB and unmapped it again, each mmap still cost the same 19 ms. The first
// version of this test measured "alone" after a trial mapping and reported the
// two arms equal and slow, which is how that half was found.
//
// The walks now read per-leaf occupancy bitmaps, so a region costs a few words
// however many pages it has, and an empty leaf costs nothing. What this asserts
// is the property, not a speed: small mmaps beside a big mapping, and after
// one, cost about what they cost in an address space that never had one.
//
// A finite RLIMIT_AS was a third route to the same cost: every mmap counted
// the address space by walking it, to compare with the limit. With the bitmaps
// in and that walk still there, `ulimit -v` alone made each small mmap beside
// the 2 GiB mapping cost 49 ms. The total is a running count now, so the last
// arm repeats "beside" under a (generous) finite RLIMIT_AS. RLIMIT_DATA is
// left alone: it still walks, and the small mappings here are data.
//
// Each round runs in a freshly forked child -- a fork builds the child's page
// table from the parent's mapped pages only, so it starts with no empty leaves
// -- which measures the three arms in turn. Rounds interleave the arms in time,
// so a host that slows down part-way through slows all three alike, and the
// arms are compared by median. The bound is deliberately loose -- ten times
// the baseline, and never less than half a millisecond above it -- because an
// emulated guest on a loaded host is noisy. The failure it exists to catch is
// two to three orders of magnitude, not a percentage.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#define ROUNDS 7
#define SMALL 200
#define RATIO_BOUND 10.0
#define ABS_SLACK_MS 0.5

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double) t.tv_sec * 1000.0 + (double) t.tv_nsec / 1e6;
}

// Milliseconds per mmap(NULL, 4096), over SMALL of them. Unmapped again
// afterwards, so every round starts from the same address space.
static double time_small_mmaps(void) {
    static void *maps[SMALL];
    double t0 = now_ms();
    for (int i = 0; i < SMALL; i++) {
        maps[i] = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (maps[i] == MAP_FAILED) {
            printf("FAIL small mmap %d: %s\n", i, strerror(errno));
            failures_total++;
            for (int j = 0; j < i; j++)
                munmap(maps[j], 4096);
            return -1;
        }
    }
    double per = (now_ms() - t0) / SMALL;
    for (int i = 0; i < SMALL; i++)
        munmap(maps[i], 4096);
    return per;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *) a, y = *(const double *) b;
    return x < y ? -1 : x > y;
}

static double median(double *v, int n) {
    qsort(v, (size_t) n, sizeof(*v), cmp_double);
    return v[n / 2];
}

// A memfd where the kernel headers know the call (what .NET maps), and an
// unlinked /tmp file otherwise -- the oldest test root predates memfd in its
// headers, and a file mapping takes the same path through the kernel.
static int big_fd(const char **kind) {
#ifdef SYS_memfd_create
    int fd = (int) syscall(SYS_memfd_create, "mmap_hole_scaling", 0);
    if (fd >= 0) {
        *kind = "memfd";
        return fd;
    }
#endif
    char path[] = "/tmp/mmap_hole_scaling.XXXXXX";
    int fd2 = mkstemp(path);
    if (fd2 >= 0)
        unlink(path);
    *kind = "file";
    return fd2;
}

// Is [p, p + len) one region of /proc/self/maps? The witness that the big
// mapping is really there, with page-table entries, for the whole of the
// measurement -- a probe that measured nothing would pass.
static int maps_has_region(void *p, size_t len) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (f == NULL)
        return -1;
    char line[512];
    int found = 0;
    uintptr_t want_lo = (uintptr_t) p, want_hi = want_lo + len;
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned long long lo, hi;
        if (sscanf(line, "%llx-%llx", &lo, &hi) == 2 &&
                (uintptr_t) lo <= want_lo && (uintptr_t) hi >= want_hi) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

enum { ALONE, BESIDE, AFTER, LIMITED, ARMS };
static const char *const arm_name[ARMS] = {
    "alone", "beside", "after", "under RLIMIT_AS, beside",
};

// One round, in a child whose page table holds only what it inherited: the
// arms, and whether the big mapping was really there. Written to `out` as ARMS
// doubles and an int; a negative time means that arm failed.
static void round_child(int fd, size_t big, int out) {
    struct { double ms[ARMS]; int seen; } r = { { -1, -1, -1, -1 }, 0 };
    r.ms[ALONE] = time_small_mmaps();
    void *p = mmap(NULL, big, PROT_NONE, MAP_SHARED, fd, 0);
    if (p != MAP_FAILED) {
        r.seen = maps_has_region(p, big);
        r.ms[BESIDE] = time_small_mmaps();
        munmap(p, big);
        r.ms[AFTER] = time_small_mmaps();
    }
    // Last, because it cannot be undone for this child. Finite but far above
    // anything mapped here: 3.5 GiB where the address space is 4 GiB, 1 TiB
    // otherwise. Only the soft limit, so it is always permitted.
    struct rlimit rl;
    if (r.seen == 1 && getrlimit(RLIMIT_AS, &rl) == 0) {
        rlim_t want = sizeof(void *) == 8 ? (rlim_t) 1 << 40 : (rlim_t) 0xe0000000u;
        if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < want)
            want = rl.rlim_max;
        rl.rlim_cur = want;
        if (setrlimit(RLIMIT_AS, &rl) == 0 &&
                (p = mmap(NULL, big, PROT_NONE, MAP_SHARED, fd, 0)) != MAP_FAILED) {
            r.ms[LIMITED] = time_small_mmaps();
            munmap(p, big);
        }
    }
    ssize_t n = write(out, &r, sizeof(r));
    _exit(n == (ssize_t) sizeof(r) ? 0 : 1);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(600));

    const char *kind = "?";
    int fd = big_fd(&kind);
    if (fd < 0) {
        printf("FAIL no memfd or temporary file: %s\n", strerror(errno));
        return finish_suite("mmap_hole_scaling");
    }

    // As big as the address space comfortably allows: 2 GiB where pointers are
    // 64-bit, less in a 4 GiB one. Halved on ENOMEM, down to 256 MiB, which
    // still cost 18x the baseline before the fix. Sized in a child, so the
    // parent -- which every round forks from -- never maps it.
    size_t big = sizeof(void *) == 8 ? (size_t) 2 << 30 : (size_t) 1 << 30;
    for (; big >= ((size_t) 256 << 20); big /= 2) {
        if (ftruncate(fd, (off_t) big) != 0)
            continue;
        pid_t pid = fork();
        if (pid == 0) {
            void *p = mmap(NULL, big, PROT_NONE, MAP_SHARED, fd, 0);
            _exit(p == MAP_FAILED ? 1 : 0);
        }
        int st = 0;
        if (pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0)
            break;
    }
    if (big < ((size_t) 256 << 20)) {
        printf("FAIL could not map a %s of 256 MiB or more\n", kind);
        close(fd);
        return finish_suite("mmap_hole_scaling");
    }

    double ms[ARMS][ROUNDS];
    int rounds = 0;
    for (int r = 0; r < ROUNDS; r++) {
        int pfd[2];
        if (pipe(pfd) != 0) {
            printf("FAIL pipe: %s\n", strerror(errno));
            failures_total++;
            break;
        }
        pid_t pid = fork();
        if (pid == 0) {
            close(pfd[0]);
            round_child(fd, big, pfd[1]);
        }
        close(pfd[1]);
        struct { double ms[ARMS]; int seen; } res;
        ssize_t n = pid > 0 ? read(pfd[0], &res, sizeof(res)) : -1;
        close(pfd[0]);
        int st = 0;
        if (pid > 0)
            waitpid(pid, &st, 0);
        if (n != (ssize_t) sizeof(res)) {
            printf("FAIL round %d: child reported nothing (status %#x)\n", r, st);
            failures_total++;
            break;
        }
        // The witness: a round in which the big mapping was not in
        // /proc/self/maps measured nothing, and must not pass.
        if (res.seen != 1) {
            printf("FAIL round %d: the %zu MiB %s mapping is not in /proc/self/maps (%d)\n",
                   r, big >> 20, kind, res.seen);
            failures_total++;
            break;
        }
        int bad = 0;
        for (int a = 0; a < ARMS; a++) {
            if (res.ms[a] < 0)
                bad = 1;
            ms[a][r] = res.ms[a];
        }
        if (bad) {
            printf("FAIL round %d: an arm could not be measured\n", r);
            failures_total++;
            break;
        }
        test_logf("  round %d: %.4f ms alone, %.4f beside, %.4f after %zu MiB, "
                  "%.4f beside under RLIMIT_AS\n", r, res.ms[ALONE], res.ms[BESIDE],
                  res.ms[AFTER], big >> 20, res.ms[LIMITED]);
        rounds++;
    }
    close(fd);

    if (failures_total == 0 && rounds == ROUNDS) {
        double med[ARMS];
        for (int a = 0; a < ARMS; a++)
            med[a] = median(ms[a], ROUNDS);
        double bound = med[ALONE] * RATIO_BOUND;
        if (bound < med[ALONE] + ABS_SLACK_MS)
            bound = med[ALONE] + ABS_SLACK_MS;
        for (int a = BESIDE; a < ARMS; a++) {
            int ok = med[a] <= bound;
            test_log_if(!ok, "  median mmap(NULL, 4096) %s a %zu MiB %s mapping: %.4f ms "
                        "vs %.4f ms alone (bound %.4f ms, %.1fx)\n",
                        arm_name[a], big >> 20, kind, med[a], med[ALONE], bound,
                        med[ALONE] > 0 ? med[a] / med[ALONE] : 0.0);
            if (!ok) {
                printf("FAIL mmap(NULL) %s a %zu MiB mapping: %.4f ms vs %.4f ms alone\n",
                       arm_name[a], big >> 20, med[a], med[ALONE]);
                failures_total++;
            }
        }
    }
    return finish_suite("mmap_hole_scaling");
}
