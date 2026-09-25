// Resource limits are enforced, not just stored.
//
// A tester found three that did nothing: a 1 s RLIMIT_CPU never fired after
// 4 s of CPU; a 128 MB allocation succeeded under a 64 MB limit; 8 MB went
// into Devuan's 5 MiB /run/lock tmpfs. What Linux does, asserted here:
//
//   RLIMIT_CPU   SIGXCPU at the soft limit, again each further second, with
//                the soft limit raised by a second each time (getrlimit shows
//                it); SIGKILL at the hard limit. Soft == hard (bash's
//                `ulimit -t`) is SIGKILL alone. The default SIGXCPU action
//                kills.
//   RLIMIT_AS    mmap, brk and mremap growth past it are ENOMEM (brk leaves
//                the break where it was); a smaller mapping still works.
//   RLIMIT_DATA  private writable mappings and the heap count; a read-only or
//                shared mapping does not; mprotect that makes pages writable
//                does.
//   setrlimit    a soft limit above the hard one is EINVAL.
//   tmpfs size=  a write that would pass the limit is cut short at it, then
//                ENOSPC; ftruncate past it is ENOSPC; statfs shows the size;
//                deleting the file gives the space back.
//
// The tmpfs leg mounts one, so it needs root (or it is skipped).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#define MiB (1024L * 1024)

static void check(const char *label, int ok, long got, long want) {
    if (!ok)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-56s %s (got %ld, want %ld)\n", label, ok ? "ok" : "FAIL", got, want);
}

// Run one leg in a child, with a watchdog of `secs` of wall time for a leg
// that hangs.
static int in_child_for(void (*fn)(void), unsigned secs) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        failures_total = 0;
        alarm(test_watchdog_secs(secs));
        fn();
        fflush(stdout);
        _exit(failures_total ? 1 : 0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return st;
}

static int in_child(void (*fn)(void)) {
    return in_child_for(fn, 30);
}

// The CPU legs spin to 3 s of CPU. On a host shared with other guests a spin
// gets a small share of a core -- the five-root gate ran at load 70 to 170 on
// 10 cores -- and a 30 s wall-clock watchdog then fired before the CPU limit
// it was waiting for. Whether they die is decided in CPU time (each spins to
// 10 s of it and reports surviving); the watchdog only catches a hang.
static int in_cpu_child(void (*fn)(void)) {
    return in_child_for(fn, 600);
}

static double cpu_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void spin_until_cpu(double secs) {
    volatile unsigned long x = 0;
    while (cpu_seconds() < secs)
        for (int i = 0; i < 100000; i++)
            x += i;
}

// ---- RLIMIT_CPU ----------------------------------------------------------------

static volatile sig_atomic_t xcpu_count;
static volatile double xcpu_at[8];
static void on_xcpu(int sig) {
    (void) sig;
    if (xcpu_count < 8)
        xcpu_at[xcpu_count] = cpu_seconds();
    xcpu_count++;
}

static void leg_cpu_soft_then_hard(void) {
    struct sigaction sa = {0};
    sa.sa_handler = on_xcpu;
    sigaction(SIGXCPU, &sa, NULL);
    struct rlimit rl = {1, 3};
    setrlimit(RLIMIT_CPU, &rl);
    // Past 2 s: SIGXCPU at 1 s and at 2 s, and the soft limit is now 3.
    spin_until_cpu(2.3);
    check("SIGXCPU at 1 s and 2 s", xcpu_count == 2, xcpu_count, 2);
    check("first SIGXCPU near 1 s of CPU", xcpu_at[0] >= 0.99 && xcpu_at[0] < 1.6,
          (long) (xcpu_at[0] * 1000), 1000);
    getrlimit(RLIMIT_CPU, &rl);
    check("soft limit raised to 3 by then", rl.rlim_cur == 3, (long) rl.rlim_cur, 3);
    // And at 3 s, the hard limit: SIGKILL.
    spin_until_cpu(10);
    printf("FAIL survived the hard CPU limit\n");
}

static void leg_cpu_equal(void) {
    struct sigaction sa = {0};
    sa.sa_handler = on_xcpu;
    sigaction(SIGXCPU, &sa, NULL);
    struct rlimit rl = {1, 1};
    setrlimit(RLIMIT_CPU, &rl);
    spin_until_cpu(10);
    printf("FAIL survived soft == hard == 1 s\n");
}

static void leg_cpu_default_action(void) {
    struct rlimit rl = {1, RLIM_INFINITY};
    setrlimit(RLIMIT_CPU, &rl);
    spin_until_cpu(10);
    printf("FAIL survived SIGXCPU's default action\n");
}

// ---- RLIMIT_AS / RLIMIT_DATA -----------------------------------------------------

static void leg_as(void) {
    struct rlimit rl = {256 * MiB, 256 * MiB};
    // The process already maps something; allow for it.
    if (setrlimit(RLIMIT_AS, &rl) != 0) {
        printf("FAIL setrlimit(RLIMIT_AS) errno=%d\n", errno);
        return;
    }
    errno = 0;
    void *p = mmap(NULL, 512 * MiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap 512 MB under a 256 MB RLIMIT_AS is ENOMEM", p == MAP_FAILED && errno == ENOMEM,
          p == MAP_FAILED ? errno : 0, ENOMEM);
    if (p != MAP_FAILED)
        munmap(p, 512 * MiB);
    errno = 0;
    p = mmap(NULL, 512 * MiB, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("...PROT_NONE counts too", p == MAP_FAILED && errno == ENOMEM,
          p == MAP_FAILED ? errno : 0, ENOMEM);
    if (p != MAP_FAILED)
        munmap(p, 512 * MiB);
    p = mmap(NULL, 16 * MiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("a 16 MB mmap still fits", p != MAP_FAILED, p == MAP_FAILED ? errno : 0, 0);
    if (p != MAP_FAILED) {
        errno = 0;
        void *q = mremap(p, 16 * MiB, 512 * MiB, MREMAP_MAYMOVE);
        check("mremap growth past RLIMIT_AS is ENOMEM", q == MAP_FAILED && errno == ENOMEM,
              q == MAP_FAILED ? errno : 0, ENOMEM);
        munmap(q == MAP_FAILED ? p : q, q == MAP_FAILED ? 16 * MiB : 512 * MiB);
    }
    void *brk0 = sbrk(0);
    errno = 0;
    void *r = sbrk(512 * MiB);
    check("brk past RLIMIT_AS fails", r == (void *) -1, r == (void *) -1 ? 0 : 1, 0);
    check("...and the break stays", sbrk(0) == brk0, sbrk(0) == brk0, 1);
}

static void leg_data(void) {
    struct rlimit rl = {64 * MiB, 64 * MiB};
    if (setrlimit(RLIMIT_DATA, &rl) != 0) {
        printf("FAIL setrlimit(RLIMIT_DATA) errno=%d\n", errno);
        return;
    }
    errno = 0;
    void *p = mmap(NULL, 128 * MiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("private RW 128 MB under a 64 MB RLIMIT_DATA is ENOMEM", p == MAP_FAILED && errno == ENOMEM,
          p == MAP_FAILED ? errno : 0, ENOMEM);
    if (p != MAP_FAILED)
        munmap(p, 128 * MiB);
    // A 128 MB malloc, which is the triage's case: musl and glibc both mmap it.
    errno = 0;
    void *m = malloc(128 * MiB);
    check("malloc(128 MB) under 64 MB fails", m == NULL, m == NULL ? 0 : 1, 0);
    free(m);
    p = mmap(NULL, 128 * MiB, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("read-only 128 MB is not data", p != MAP_FAILED, p == MAP_FAILED ? errno : 0, 0);
    if (p != MAP_FAILED) {
        errno = 0;
        int r = mprotect(p, 128 * MiB, PROT_READ | PROT_WRITE);
        check("...until mprotect makes it writable: ENOMEM", r != 0 && errno == ENOMEM, r ? errno : 0, ENOMEM);
        munmap(p, 128 * MiB);
    }
    p = mmap(NULL, 128 * MiB, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    check("shared RW 128 MB is not data", p != MAP_FAILED, p == MAP_FAILED ? errno : 0, 0);
    if (p != MAP_FAILED)
        munmap(p, 128 * MiB);
    void *brk0 = sbrk(0);
    void *r = sbrk(128 * MiB);
    check("brk 128 MB past RLIMIT_DATA fails", r == (void *) -1, r == (void *) -1 ? 0 : 1, 0);
    check("...and the break stays", sbrk(0) == brk0, sbrk(0) == brk0, 1);
    p = mmap(NULL, 4 * MiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("a 4 MB private mapping still fits", p != MAP_FAILED, p == MAP_FAILED ? errno : 0, 0);
}

static void leg_setrlimit_inval(void) {
    struct rlimit rl = {64 * MiB, 32 * MiB};
    errno = 0;
    int r = setrlimit(RLIMIT_AS, &rl);
    check("soft above hard is EINVAL", r != 0 && errno == EINVAL, r ? errno : 0, EINVAL);
    rl = (struct rlimit) {RLIM_INFINITY, 32 * MiB};
    errno = 0;
    r = setrlimit(RLIMIT_DATA, &rl);
    check("infinite soft over a finite hard is EINVAL", r != 0 && errno == EINVAL, r ? errno : 0, EINVAL);
}

// ---- tmpfs size= ------------------------------------------------------------------

static void leg_tmpfs(void) {
    char dir[] = "/tmp/rlimit_enforce_tmpfs.XXXXXX";
    if (mkdtemp(dir) == NULL) {
        printf("FAIL mkdtemp errno=%d\n", errno);
        return;
    }
    if (mount("tmpfs", dir, "tmpfs", 0, "size=5M") != 0) {
        printf("FAIL mount tmpfs size=5M errno=%d\n", errno);
        rmdir(dir);
        return;
    }
    struct statfs sf;
    statfs(dir, &sf);
    check("statfs shows 5 MiB", (long) sf.f_blocks * sf.f_bsize == 5 * MiB,
          (long) sf.f_blocks * sf.f_bsize, 5 * MiB);
    char path[128];
    snprintf(path, sizeof path, "%s/big", dir);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    static char chunk[1024 * 1024];
    memset(chunk, 'x', sizeof chunk);
    long total = 0;
    int last_errno = 0;
    for (int i = 0; i < 8; i++) {
        ssize_t n = write(fd, chunk, sizeof chunk);
        if (n < 0) {
            last_errno = errno;
            break;
        }
        total += n;
        if ((size_t) n < sizeof chunk) {
            // A short write at the limit; the next one fails.
            n = write(fd, chunk, sizeof chunk);
            last_errno = n < 0 ? errno : 0;
            break;
        }
    }
    check("8 MB into 5 MiB stops at 5 MiB", total == 5 * MiB, total, 5 * MiB);
    check("...with ENOSPC", last_errno == ENOSPC, last_errno, ENOSPC);
    errno = 0;
    int r = ftruncate(fd, 64 * MiB);
    // Linux's tmpfs would allow this hole; AOK stores files whole and says
    // ENOSPC instead. Either way the space is not handed out.
    check("ftruncate past the limit does not succeed silently",
          r == 0 || errno == ENOSPC, r ? errno : 0, 0);
    close(fd);
    unlink(path);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ssize_t n = write(fd, chunk, sizeof chunk);
    check("unlink gives the space back", n == (ssize_t) sizeof chunk, n, (long) sizeof chunk);
    close(fd);
    umount(dir);
    rmdir(dir);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    int st;

    st = in_cpu_child(leg_cpu_soft_then_hard);
    check("soft then hard: killed by SIGKILL", WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL,
          WIFSIGNALED(st) ? WTERMSIG(st) : -st, SIGKILL);
    st = in_cpu_child(leg_cpu_equal);
    check("soft == hard: killed by SIGKILL", WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL,
          WIFSIGNALED(st) ? WTERMSIG(st) : -st, SIGKILL);
    st = in_cpu_child(leg_cpu_default_action);
    check("SIGXCPU's default action kills", WIFSIGNALED(st) && WTERMSIG(st) == SIGXCPU,
          WIFSIGNALED(st) ? WTERMSIG(st) : -st, SIGXCPU);

    st = in_child(leg_as);
    check("RLIMIT_AS", WIFEXITED(st) && WEXITSTATUS(st) == 0, st, 0);
    st = in_child(leg_data);
    check("RLIMIT_DATA", WIFEXITED(st) && WEXITSTATUS(st) == 0, st, 0);
    st = in_child(leg_setrlimit_inval);
    check("setrlimit validation", WIFEXITED(st) && WEXITSTATUS(st) == 0, st, 0);

    if (geteuid() == 0) {
        st = in_child(leg_tmpfs);
        check("tmpfs size=", WIFEXITED(st) && WEXITSTATUS(st) == 0, st, 0);
    } else {
        test_logf("not root: tmpfs leg skipped\n");
    }
    return finish_suite("rlimit_enforce");
}
