// Reading /proc/meminfo must not cost more because something big is mapped,
// and its Shmem, AnonPages and Mapped lines must come back to where they were
// once the mappings that moved them are gone.
//
// AOK summed those three lines by walking every page of every address space,
// under each one's read lock. The .NET GC reads MemAvailable on every
// collection, and .NET maps its code heap twice (read-write and read-execute)
// from one MAP_SHARED memfd, so each read visited every page of it twice while
// an mmap on another thread waited for the write lock. Measured on an amd64
// guest, one open/read/close of /proc/meminfo:
//
//   nothing big mapped                           0.5 ms
//   beside a 2 GiB memfd mapped RW and RX       54 ms (200-1200 on a busy host)
//
// Linux 6.12 (camd) takes 0.011 ms either way. The lines are counters now,
// kept per address space where its page-table entries come and go, and the
// read is 0.05-0.06 ms both ways.
//
// What this asserts, so that it holds on Linux too:
//
//   scaling  -- rounds interleave reads alone, beside a child holding the
//               double mapping, and after it has gone, compared by median. The
//               bound is loose (4x, and never less than 2 ms above alone) for a
//               slow device on a busy host; the failure it exists to catch was
//               100x. The mappings must be in the child's /proc/self/maps while
//               "beside" is timed, or the round measured nothing.
//   touched  -- 32 MiB touched in a private anonymous, a shared anonymous and
//               a single memfd mapping raises AnonPages, Shmem and Mapped by
//               about 32 MiB, and unmapping takes it back off.
//   return   -- each other way AOK publishes or clears a page-table entry (a
//               fork's copy and the child's exit, a COW break, MADV_WIPEONFORK,
//               an mremap move, an mremap alias, an mprotect that commits a
//               PROT_NONE range) leaves all three where they started once it
//               is undone. A counter that one of them forgot moves for good,
//               or wraps to an absurd figure.
//
// The values are system-wide, so on Linux other processes move them too. Each
// value leg is retried a few times and passes on the first clean attempt; a
// real counter bug is wrong on every attempt.
//
// What AOK counts is not yet what Linux counts, which is why the legs above
// ask only what both agree on: AOK counts a page from mmap rather than from
// first touch, counts a memfd page once per mapping, and files memfd pages
// under Mapped only (Linux: Shmem and Mapped). docs/TODO.md has the table.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#ifndef MADV_WIPEONFORK
#define MADV_WIPEONFORK 18
#endif
#ifndef MREMAP_MAYMOVE
#define MREMAP_MAYMOVE 1
#endif

#define ROUNDS 7
#define READS 9
#define RATIO_BOUND 4.0
#define ABS_SLACK_MS 2.0
#define LEG_MB 32
#define LEG_BYTES ((size_t) LEG_MB << 20)
#define TOL_KB 4096L
#define ATTEMPTS 5

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double) t.tv_sec * 1000.0 + (double) t.tv_nsec / 1e6;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *) a, y = *(const double *) b;
    return x < y ? -1 : x > y;
}

static double median(double *v, int n) {
    qsort(v, (size_t) n, sizeof(*v), cmp_double);
    return v[n / 2];
}

// One open/read/close of /proc/meminfo, as a GC does it. Negative on error.
static double read_once_ms(void) {
    char buf[8192];
    double t0 = now_ms();
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n, total = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        total += n;
    close(fd);
    double ms = now_ms() - t0;
    return total > 0 ? ms : -1;
}

static double read_median_ms(void) {
    double v[READS];
    for (int i = 0; i < READS; i++) {
        v[i] = read_once_ms();
        if (v[i] < 0)
            return -1;
    }
    return median(v, READS);
}

struct mi { long shmem, anon, mapped; };

static int meminfo(struct mi *m) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (f == NULL)
        return -1;
    char line[256];
    m->shmem = m->anon = m->mapped = -1;
    while (fgets(line, sizeof(line), f) != NULL) {
        long v;
        if (sscanf(line, "Shmem: %ld", &v) == 1)
            m->shmem = v;
        else if (sscanf(line, "AnonPages: %ld", &v) == 1)
            m->anon = v;
        else if (sscanf(line, "Mapped: %ld", &v) == 1)
            m->mapped = v;
    }
    fclose(f);
    return (m->shmem < 0 || m->anon < 0 || m->mapped < 0) ? -1 : 0;
}

static int memfd(const char *name) {
#ifdef SYS_memfd_create
    int fd = (int) syscall(SYS_memfd_create, name, 0);
    if (fd >= 0)
        return fd;
#endif
    char path[] = "/tmp/meminfo_scaling.XXXXXX";
    int fd2 = mkstemp(path);
    if (fd2 >= 0)
        unlink(path);
    return fd2;
}

// Is [p, p + len) inside one region of /proc/self/maps?
static int maps_has_region(void *p, size_t len) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (f == NULL)
        return 0;
    char line[512];
    int found = 0;
    uintptr_t lo_want = (uintptr_t) p, hi_want = lo_want + len;
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned long long lo, hi;
        if (sscanf(line, "%llx-%llx", &lo, &hi) == 2 &&
                (uintptr_t) lo <= lo_want && (uintptr_t) hi >= hi_want) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static void touch(volatile char *p, size_t len) {
    for (size_t i = 0; i < len; i += 4096)
        p[i] = 1;
}

// ---- scaling -------------------------------------------------------------

enum { ALONE, BESIDE, AFTER, ARMS };
static const char *const arm_name[ARMS] = { "alone", "beside", "after" };

// A child maps the memfd twice, RW and RX as .NET does, and holds the mappings
// until the parent closes *go. The reader never maps anything big itself, and
// that matters: AOK's page-table leaves outlive their mappings, and the old
// walk visited every slot of every leaf a process had ever used, so a reader
// that had once mapped the memfd paid for it on every later read. The first
// version of this test mapped in the reader, measured "alone" at 73 ms on the
// unfixed kernel -- as slow as "beside" -- and passed. A child's leaves go
// with its address space when it exits.
//
// Returns the child's pid, or -1, with *seen set to whether both mappings are
// in the child's /proc/self/maps: the witness that "beside" had something to
// be beside.
static pid_t hold_big(int fd, size_t big, int *go, int *seen) {
    int ready[2], cont[2];
    *seen = 0;
    if (pipe(ready) != 0)
        return -1;
    if (pipe(cont) != 0) {
        close(ready[0]);
        close(ready[1]);
        return -1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(ready[0]);
        close(cont[1]);
        char status = 'F';
        char *rw = mmap(NULL, big, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        char *rx = mmap(NULL, big, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
        if (rw != MAP_FAILED && rx != MAP_FAILED)
            status = maps_has_region(rw, big) && maps_has_region(rx, big) ? 'Y' : 'N';
        if (write(ready[1], &status, 1) != 1)
            _exit(1);
        char c;
        while (read(cont[0], &c, 1) > 0)   // until the parent closes its end
            ;
        _exit(0);
    }
    close(ready[1]);
    close(cont[0]);
    char status = 0;
    if (pid < 0 || read(ready[0], &status, 1) != 1)
        status = 0;
    close(ready[0]);
    *go = cont[1];
    *seen = status == 'Y';
    if (pid > 0 && status == 'F') {
        close(cont[1]);
        waitpid(pid, NULL, 0);
        return -1;
    }
    return pid;
}

static void release_big(pid_t pid, int go) {
    close(go);
    if (pid > 0)
        waitpid(pid, NULL, 0);
}

static void scaling(void) {
    int fd = memfd("meminfo_scaling");
    if (fd < 0) {
        printf("FAIL scaling: no memfd or temporary file: %s\n", strerror(errno));
        failures_total++;
        return;
    }
    // 1 GiB mapped twice where pointers are 64-bit; 512 MiB twice in a 4 GiB
    // address space. Halved when the child cannot map it, down to 128 MiB.
    size_t big = sizeof(void *) == 8 ? (size_t) 1 << 30 : (size_t) 512 << 20;
    int go = -1, seen = 0;
    pid_t pid = -1;
    for (; big >= ((size_t) 128 << 20); big /= 2) {
        if (ftruncate(fd, (off_t) big) != 0)
            continue;
        pid = hold_big(fd, big, &go, &seen);
        if (pid > 0)
            break;
    }
    if (pid <= 0) {
        printf("FAIL scaling: could not map a memfd of 128 MiB or more twice\n");
        failures_total++;
        close(fd);
        return;
    }
    release_big(pid, go);

    double ms[ARMS][ROUNDS];
    for (int r = 0; r < ROUNDS; r++) {
        ms[ALONE][r] = read_median_ms();
        pid = hold_big(fd, big, &go, &seen);
        if (pid <= 0 || !seen) {
            printf("FAIL scaling round %d: the 2 x %zu MiB mappings are not in the "
                   "holder's /proc/self/maps\n", r, big >> 20);
            failures_total++;
            if (pid > 0)
                release_big(pid, go);
            break;
        }
        ms[BESIDE][r] = read_median_ms();
        release_big(pid, go);
        ms[AFTER][r] = read_median_ms();
        if (ms[ALONE][r] < 0 || ms[BESIDE][r] < 0 || ms[AFTER][r] < 0) {
            printf("FAIL scaling round %d: /proc/meminfo could not be read\n", r);
            failures_total++;
            break;
        }
        test_logf("  round %d: %.4f ms alone, %.4f beside 2 x %zu MiB, %.4f after\n",
                  r, ms[ALONE][r], ms[BESIDE][r], big >> 20, ms[AFTER][r]);
    }
    close(fd);
    if (failures_total != 0)
        return;

    double med[ARMS];
    for (int a = 0; a < ARMS; a++)
        med[a] = median(ms[a], ROUNDS);
    double bound = med[ALONE] * RATIO_BOUND;
    if (bound < med[ALONE] + ABS_SLACK_MS)
        bound = med[ALONE] + ABS_SLACK_MS;
    for (int a = BESIDE; a < ARMS; a++) {
        int ok = med[a] <= bound;
        test_log_if(!ok, "  median /proc/meminfo read %s a memfd mapped twice (2 x %zu MiB): "
                    "%.4f ms vs %.4f ms alone (bound %.4f ms, %.1fx)\n", arm_name[a],
                    big >> 20, med[a], med[ALONE], bound,
                    med[ALONE] > 0 ? med[a] / med[ALONE] : 0.0);
        if (!ok) {
            printf("FAIL /proc/meminfo read %s 2 x %zu MiB of memfd: %.4f ms vs %.4f ms alone\n",
                   arm_name[a], big >> 20, med[a], med[ALONE]);
            failures_total++;
        }
    }
}

// ---- values --------------------------------------------------------------

static long field(const struct mi *m, int which) {
    return which == 0 ? m->shmem : which == 1 ? m->anon : m->mapped;
}
static const char *const field_name[3] = { "Shmem", "AnonPages", "Mapped" };

// Map LEG_BYTES and touch it, and expect `which` to rise by about that much;
// unmap, and expect every field back where it started.
typedef void *(*map_fn)(int *fd);

static void *map_private_anon(int *fd) {
    *fd = -1;
    return mmap(NULL, LEG_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}
static void *map_shared_anon(int *fd) {
    *fd = -1;
    return mmap(NULL, LEG_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
}
static void *map_memfd(int *fd) {
    *fd = memfd("meminfo_scaling_leg");
    if (*fd < 0 || ftruncate(*fd, (off_t) LEG_BYTES) != 0)
        return MAP_FAILED;
    return mmap(NULL, LEG_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0);
}

static int near(long got, long want) {
    return got >= want - TOL_KB && got <= want + TOL_KB;
}

static void touched_leg(const char *name, map_fn map, int which) {
    const long want = (long) (LEG_BYTES >> 10);
    long rise = 0, back[3] = { 0, 0, 0 };
    for (int attempt = 0; attempt < ATTEMPTS; attempt++) {
        struct mi before, up, after;
        int fd = -1;
        if (meminfo(&before) != 0) {
            printf("FAIL %s: /proc/meminfo has no Shmem/AnonPages/Mapped\n", name);
            failures_total++;
            return;
        }
        char *p = map(&fd);
        if (p == MAP_FAILED) {
            printf("FAIL %s: mmap: %s\n", name, strerror(errno));
            failures_total++;
            if (fd >= 0)
                close(fd);
            return;
        }
        touch(p, LEG_BYTES);
        meminfo(&up);
        munmap(p, LEG_BYTES);
        if (fd >= 0)
            close(fd);
        meminfo(&after);
        rise = field(&up, which) - field(&before, which);
        int ok = near(rise, want);
        for (int f = 0; f < 3; f++) {
            back[f] = field(&after, f) - field(&before, f);
            if (!near(back[f], 0))
                ok = 0;
        }
        test_logf("  %s attempt %d: %s %+ld kB touched; after unmap Shmem %+ld AnonPages "
                  "%+ld Mapped %+ld kB\n", name, attempt, field_name[which], rise,
                  back[0], back[1], back[2]);
        if (ok)
            return;
    }
    printf("FAIL %s: %s rose %ld kB for %ld kB touched (want within %ld); after unmap "
           "Shmem %+ld AnonPages %+ld Mapped %+ld kB (want 0)\n", name, field_name[which],
           rise, want, TOL_KB, back[0], back[1], back[2]);
    failures_total++;
}

// ---- return legs: do something, undo it, and all three must be back -------

// An operation returns 0 when done and undone, -1 when it could not be done,
// and 1 when a reading it took on the way was out of range -- a failed
// attempt, retried like any other, and described in leg_note.
typedef int (*op_fn)(void);
static char leg_note[256];

// A counter the fork copy forgot would never show in a return leg: the
// child's page table, and its miscount with it, go when the child exits. So
// this one also reads while the child is alive, after it has broken half of
// what it shares. Linux has then added the half it copied (the shared pages
// were counted once, in the parent); AOK counts each address space's entries,
// so it has added the child's whole copy. Both are at least half -- and a fork
// that published the child's entries uncounted adds nothing at all.
static int op_fork_cow_exit(void) {
    char *p = mmap(NULL, LEG_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return -1;
    touch(p, LEG_BYTES);
    int ready[2], cont[2];
    if (pipe(ready) != 0 || pipe(cont) != 0)
        return -1;
    struct mi before, during;
    if (meminfo(&before) != 0)
        return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(ready[0]);
        close(cont[1]);
        touch(p, LEG_BYTES / 2);    // break half of it, leave the rest shared
        char c = 'r';
        if (write(ready[1], &c, 1) != 1)
            _exit(1);
        while (read(cont[0], &c, 1) > 0)
            ;
        _exit(0);
    }
    close(ready[1]);
    close(cont[0]);
    char c;
    int got = pid > 0 && read(ready[0], &c, 1) == 1;
    int read_ok = got && meminfo(&during) == 0;
    close(ready[0]);
    close(cont[1]);
    int st = 0;
    if (pid < 0 || waitpid(pid, &st, 0) != pid || !WIFEXITED(st) || !read_ok)
        return -1;
    touch(p, LEG_BYTES);    // the parent's own break of what it shared
    if (munmap(p, LEG_BYTES) != 0)
        return -1;
    long rise = during.anon - before.anon;
    const long half = (long) (LEG_BYTES >> 10) / 2, whole = (long) (LEG_BYTES >> 10);
    test_logf("  fork: AnonPages %+ld kB with the child alive, half broken\n", rise);
    if (rise < half - TOL_KB || rise > whole + TOL_KB) {
        snprintf(leg_note, sizeof(leg_note), "with the child alive and half of %ld kB "
                 "broken, AnonPages rose %+ld kB (want %ld to %ld)", whole, rise,
                 half - TOL_KB, whole + TOL_KB);
        return 1;
    }
    return 0;
}

static int op_wipeonfork(void) {
    char *p = mmap(NULL, LEG_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return -1;
    touch(p, LEG_BYTES);
    if (madvise(p, LEG_BYTES, MADV_WIPEONFORK) != 0) {
        munmap(p, LEG_BYTES);
        return errno == EINVAL ? 0 : -1;    // a kernel without it: nothing to undo
    }
    pid_t pid = fork();
    if (pid == 0) {
        touch(p, LEG_BYTES);    // fresh zero pages here, not the parent's
        _exit(0);
    }
    int st = 0;
    if (pid < 0 || waitpid(pid, &st, 0) != pid || !WIFEXITED(st) || WEXITSTATUS(st) != 0)
        return -1;
    return munmap(p, LEG_BYTES);
}

static int op_mremap_move(void) {
    char *p = mmap(NULL, LEG_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return -1;
    touch(p, LEG_BYTES);
    // Grow it by moving it, which a neighbour forces if one is there and the
    // kernel may do anyway; either way the old range goes.
    char *q = mremap(p, LEG_BYTES, 2 * LEG_BYTES, MREMAP_MAYMOVE);
    if (q == MAP_FAILED) {
        munmap(p, LEG_BYTES);
        return -1;
    }
    touch(q, 2 * LEG_BYTES);
    return munmap(q, 2 * LEG_BYTES);
}

static int op_mremap_alias(void) {
    char *p = mmap(NULL, LEG_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return -1;
    touch(p, LEG_BYTES);
    // old_size 0 of a shared mapping: a second mapping of the same pages.
    char *q = mremap(p, 0, LEG_BYTES, MREMAP_MAYMOVE);
    if (q == MAP_FAILED) {
        munmap(p, LEG_BYTES);
        return -1;
    }
    touch(q, LEG_BYTES);
    if (munmap(p, LEG_BYTES) != 0)
        return -1;
    return munmap(q, LEG_BYTES);
}

static int op_mprotect_commit(void) {
    int fd = memfd("meminfo_scaling_prot");
    if (fd < 0 || ftruncate(fd, (off_t) LEG_BYTES) != 0)
        return -1;
    char *f = mmap(NULL, LEG_BYTES, PROT_NONE, MAP_SHARED, fd, 0);
    char *p = mmap(NULL, LEG_BYTES, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    close(fd);
    if (f == MAP_FAILED || p == MAP_FAILED)
        return -1;
    if (mprotect(p + LEG_BYTES / 4, LEG_BYTES / 2, PROT_READ | PROT_WRITE) != 0 ||
            mprotect(f, LEG_BYTES, PROT_READ | PROT_WRITE) != 0)
        return -1;
    touch(p + LEG_BYTES / 4, LEG_BYTES / 2);
    touch(f, LEG_BYTES);
    // Part of it first, splitting the committed middle, then the rest.
    const size_t cut = LEG_BYTES / 8 * 3;
    if (munmap(p, cut) != 0 || munmap(p + cut, LEG_BYTES - cut) != 0)
        return -1;
    return munmap(f, LEG_BYTES);
}

static void return_leg(const char *name, op_fn op) {
    long back[3] = { 0, 0, 0 };
    leg_note[0] = '\0';
    for (int attempt = 0; attempt < ATTEMPTS; attempt++) {
        struct mi before, after;
        if (meminfo(&before) != 0) {
            printf("FAIL %s: /proc/meminfo has no Shmem/AnonPages/Mapped\n", name);
            failures_total++;
            return;
        }
        leg_note[0] = '\0';
        int r = op();
        if (r < 0) {
            printf("FAIL %s: the operation failed: %s\n", name, strerror(errno));
            failures_total++;
            return;
        }
        meminfo(&after);
        int ok = r == 0;
        if (r > 0)
            test_logf("  %s attempt %d: %s\n", name, attempt, leg_note);
        for (int f = 0; f < 3; f++) {
            back[f] = field(&after, f) - field(&before, f);
            if (!near(back[f], 0))
                ok = 0;
        }
        test_logf("  %s attempt %d: Shmem %+ld AnonPages %+ld Mapped %+ld kB\n",
                  name, attempt, back[0], back[1], back[2]);
        if (ok)
            return;
    }
    if (leg_note[0] != '\0')
        printf("FAIL %s: %s\n", name, leg_note);
    else
        printf("FAIL %s: undone, but Shmem %+ld AnonPages %+ld Mapped %+ld kB (want 0 +- %ld)\n",
               name, back[0], back[1], back[2], TOL_KB);
    failures_total++;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(600));

    scaling();

    touched_leg("private anonymous", map_private_anon, 1);
    touched_leg("shared anonymous", map_shared_anon, 0);
    touched_leg("memfd", map_memfd, 2);

    return_leg("fork, COW break, exit", op_fork_cow_exit);
    return_leg("MADV_WIPEONFORK fork", op_wipeonfork);
    return_leg("mremap move", op_mremap_move);
    return_leg("mremap alias", op_mremap_alias);
    return_leg("mprotect commit", op_mprotect_commit);

    return finish_suite("meminfo_scaling");
}
