// What one process may learn about another: Linux's ptrace_may_access().
//
// AOK checked it for /proc/<pid>/mem, environ and auxv and nowhere else, so
// any user could read root's memory map (/proc/<pid>/maps, smaps), list and
// OPEN the files of every process (/proc/<pid>/fd -- a pipe, socket or deleted
// file came back as the very description its owner holds), see where it was
// (cwd, root, exe) -- and PTRACE_ATTACH to it, which is reading and writing
// its memory and registers: root for anyone. PR_SET_DUMPABLE, which ssh-agent
// and sshd use to keep keys from their own user's other processes, was
// accepted and ignored.
//
// Linux's rule, asserted here from the unprivileged side:
//   - another user's process (or one that is not dumpable): maps, smaps,
//     smaps_rollup, environ, auxv, mem, io, fd/, fdinfo/, fd/N (stat, open),
//     cwd, root, exe and ns/* links all fail EACCES; PTRACE_ATTACH and
//     PTRACE_SEIZE fail EPERM, and so does kcmp; status, cmdline and stat
//     stay readable, with stat's start-of-stack, brk, arg and env addresses 0.
//   - the caller's own user's dumpable process: all of it works.
//   - a process that set PR_SET_DUMPABLE(0), or that dropped from root to the
//     caller's uid (a change of effective ids makes it undumpable), is denied
//     like another user's -- until it execs an ordinary program again.
//
// Run as root (the AOK CLI) this forks a root victim and drops a prober to an
// unprivileged uid in-process; run unprivileged (camd, the suite's user leg)
// it uses pid 1 as the root victim and skips the legs that need root to set up.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#define UNPRIV_UID 2020
#define UNPRIV_GID 2121

#ifndef SYS_kcmp
# ifdef __NR_kcmp
#  define SYS_kcmp __NR_kcmp
# endif
#endif
#define KCMP_VM_ 1

static const char *self_path;

static void check(const char *label, int ok, long got, long want) {
    if (!ok)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-50s %s (got %ld, want %ld)\n", label, ok ? "ok" : "FAIL", got, want);
}

static char *procp(pid_t pid, const char *what) {
    static char buf[8][128];
    static int i;
    char *p = buf[i++ & 7];
    snprintf(p, 128, "/proc/%d/%s", (int) pid, what);
    return p;
}

// errno of open(path, O_RDONLY), 0 on success.
static int open_errno(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return errno;
    close(fd);
    return 0;
}

static int opendir_errno(const char *path) {
    DIR *d = opendir(path);
    if (d == NULL)
        return errno;
    closedir(d);
    return 0;
}

static int readlink_errno(const char *path) {
    char buf[256];
    return readlink(path, buf, sizeof buf) < 0 ? errno : 0;
}

static int stat_errno(const char *path) {
    struct stat st;
    return stat(path, &st) < 0 ? errno : 0;
}

// Fields 28 (startstack) and 47..51 (brk start, arg and env bounds) of
// /proc/<pid>/stat, summed: 0 when they are blanked.
static long long stat_addresses(pid_t pid) {
    char buf[2048];
    int fd = open(procp(pid, "stat"), O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    char *p = strrchr(buf, ')');
    if (p == NULL)
        return -1;
    p += 2;
    long long sum = 0;
    // p is at field 3 (state).
    for (int field = 3; *p != '\0'; field++) {
        long long v = strtoll(p, NULL, 10);
        if (field == 28 || (field >= 47 && field <= 51))
            sum += v;
        char *sp = strchr(p, ' ');
        if (sp == NULL)
            break;
        p = sp + 1;
    }
    return sum;
}

static long do_kcmp(pid_t a, pid_t b) {
#ifdef SYS_kcmp
    errno = 0;
    long r = syscall(SYS_kcmp, (long) a, (long) b, (long) KCMP_VM_, 0L, 0L);
    return r < 0 ? -errno : r;
#else
    (void) a; (void) b;
    return -ENOSYS;
#endif
}

// Every check against a process the caller must NOT be able to inspect.
// fdno is a descriptor the victim is known to hold.
static void expect_denied(const char *who, pid_t v, int fdno) {
    test_logf("-- %s (pid %d): denied --\n", who, (int) v);
    char label[128];
    static const char *files[] = {"maps", "smaps", "smaps_rollup", "environ",
                                  "auxv", "mem", "io"};
    for (unsigned i = 0; i < sizeof files / sizeof *files; i++) {
        // smaps_rollup is 4.14+, and some kernels leave one of these out;
        // ENOENT is not the answer being tested for.
        int e = open_errno(procp(v, files[i]));
        if (e == ENOENT)
            continue;
        snprintf(label, sizeof label, "%s: open %s", who, files[i]);
        check(label, e == EACCES, e, EACCES);
    }
    snprintf(label, sizeof label, "%s: opendir fd", who);
    int e = opendir_errno(procp(v, "fd"));
    check(label, e == EACCES, e, EACCES);
    snprintf(label, sizeof label, "%s: opendir fdinfo", who);
    e = opendir_errno(procp(v, "fdinfo"));
    check(label, e == EACCES, e, EACCES);

    char fdpath[64];
    snprintf(fdpath, sizeof fdpath, "fd/%d", fdno);
    snprintf(label, sizeof label, "%s: stat %s", who, fdpath);
    e = stat_errno(procp(v, fdpath));
    check(label, e == EACCES, e, EACCES);
    snprintf(label, sizeof label, "%s: open %s", who, fdpath);
    e = open_errno(procp(v, fdpath));
    check(label, e == EACCES, e, EACCES);
    snprintf(fdpath, sizeof fdpath, "fdinfo/%d", fdno);
    snprintf(label, sizeof label, "%s: open %s", who, fdpath);
    e = open_errno(procp(v, fdpath));
    check(label, e == EACCES, e, EACCES);

    static const char *links[] = {"cwd", "root", "exe", "ns/net"};
    for (unsigned i = 0; i < sizeof links / sizeof *links; i++) {
        snprintf(label, sizeof label, "%s: readlink %s", who, links[i]);
        e = readlink_errno(procp(v, links[i]));
        check(label, e == EACCES, e, EACCES);
    }

    // What stays public.
    static const char *pub[] = {"status", "cmdline", "stat", "comm"};
    for (unsigned i = 0; i < sizeof pub / sizeof *pub; i++) {
        snprintf(label, sizeof label, "%s: open %s", who, pub[i]);
        e = open_errno(procp(v, pub[i]));
        check(label, e == 0, e, 0);
    }
    snprintf(label, sizeof label, "%s: stat addresses blanked", who);
    long long sum = stat_addresses(v);
    check(label, sum == 0, (long) sum, 0);

    errno = 0;
    long r = ptrace(PTRACE_ATTACH, v, 0, 0);
    snprintf(label, sizeof label, "%s: PTRACE_ATTACH", who);
    check(label, r < 0 && errno == EPERM, r < 0 ? errno : 0, EPERM);
    if (r == 0)
        ptrace(PTRACE_DETACH, v, 0, 0);
    errno = 0;
    r = ptrace(PTRACE_SEIZE, v, 0, 0);
    snprintf(label, sizeof label, "%s: PTRACE_SEIZE", who);
    check(label, r < 0 && errno == EPERM, r < 0 ? errno : 0, EPERM);

    long k = do_kcmp(getpid(), v);
    if (k != -ENOSYS) {
        snprintf(label, sizeof label, "%s: kcmp", who);
        check(label, k == -EPERM, k, -EPERM);
    }
}

// The same list against a process the caller may inspect. `self` skips the
// attach, which Linux refuses for a thread of the caller's own process.
static void expect_allowed(const char *who, pid_t v, int self) {
    test_logf("-- %s (pid %d): allowed --\n", who, (int) v);
    char label[128];
    static const char *files[] = {"maps", "smaps", "environ", "auxv", "io"};
    for (unsigned i = 0; i < sizeof files / sizeof *files; i++) {
        snprintf(label, sizeof label, "%s: open %s", who, files[i]);
        int e = open_errno(procp(v, files[i]));
        check(label, e == 0, e, 0);
    }
    snprintf(label, sizeof label, "%s: opendir fd", who);
    int e = opendir_errno(procp(v, "fd"));
    check(label, e == 0, e, 0);
    snprintf(label, sizeof label, "%s: stat fd/0", who);
    e = stat_errno(procp(v, "fd/0"));
    check(label, e == 0, e, 0);
    snprintf(label, sizeof label, "%s: readlink cwd", who);
    e = readlink_errno(procp(v, "cwd"));
    check(label, e == 0, e, 0);
    snprintf(label, sizeof label, "%s: stat addresses shown", who);
    long long sum = stat_addresses(v);
    check(label, sum > 0, (long) sum, 1);

    if (!self) {
        errno = 0;
        long r = ptrace(PTRACE_ATTACH, v, 0, 0);
        snprintf(label, sizeof label, "%s: PTRACE_ATTACH", who);
        check(label, r == 0, r < 0 ? errno : 0, 0);
        if (r == 0) {
            int st;
            waitpid(v, &st, __WALL);
            ptrace(PTRACE_DETACH, v, 0, 0);
        }
    }
    long k = do_kcmp(getpid(), v);
    if (k != -ENOSYS) {
        snprintf(label, sizeof label, "%s: kcmp", who);
        check(label, k >= 0, k, 1);
    }
}

// A child that does `setup` and then waits to be looked at. Reports over a
// pipe that it is ready, with one byte of its own choosing.
static pid_t spawn_victim(int (*setup)(void), int *report) {
    int p[2];
    if (pipe(p) < 0)
        return -1;
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        char c = (char) setup();
        if (write(p[1], &c, 1) != 1)
            _exit(1);
        for (;;)
            pause();
    }
    close(p[1]);
    char c = 0;
    if (read(p[0], &c, 1) != 1)
        c = -1;
    close(p[0]);
    if (report != NULL)
        *report = c;
    return pid;
}

static int setup_plain(void) { return 0; }
static int setup_undumpable(void) {
    if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) < 0)
        return -2;
    return prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
}
static int setup_drop_to_unpriv(void) {
    if (setresgid(UNPRIV_GID, UNPRIV_GID, UNPRIV_GID) < 0 ||
            setresuid(UNPRIV_UID, UNPRIV_UID, UNPRIV_UID) < 0)
        return -2;
    return prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
}

// Undumpable, then an ordinary exec: dumpable again.
static pid_t spawn_reexec(void) {
    int p[2];
    if (pipe(p) < 0)
        return -1;
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
        char fdarg[16];
        snprintf(fdarg, sizeof fdarg, "%d", p[1]);
        execl(self_path, self_path, "--victim", fdarg, (char *) NULL);
        _exit(127);
    }
    close(p[1]);
    char c;
    if (read(p[0], &c, 1) != 1)
        c = -1;
    close(p[0]);
    return c == 1 ? pid : -pid;
}

static void reap(pid_t pid) {
    if (pid <= 0)
        return;
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

static void prober(pid_t root_victim, int root_fd, pid_t dropped_victim) {
    // Its own process, from inside: always.
    expect_allowed("self", getpid(), 1);

    if (root_victim > 0)
        expect_denied("root process", root_victim, root_fd);
    if (dropped_victim > 0)
        expect_denied("root dropped to my uid", dropped_victim, 0);

    pid_t same = spawn_victim(setup_plain, NULL);
    if (same > 0)
        expect_allowed("my own process", same, 0);
    reap(same);

    int dumpable = -1;
    pid_t undump = spawn_victim(setup_undumpable, &dumpable);
    check("PR_GET_DUMPABLE after PR_SET_DUMPABLE(0)", dumpable == 0, dumpable, 0);
    if (undump > 0)
        expect_denied("my own undumpable process", undump, 0);
    reap(undump);

    pid_t re = spawn_reexec();
    check("undumpable re-exec reported in", re > 0, re, 1);
    if (re > 0)
        expect_allowed("undumpable, then exec'd", re, 0);
    reap(re < 0 ? -re : re);
}

int main(int argc, char **argv) {
    self_path = argv[0];
    if (argc == 3 && strcmp(argv[1], "--victim") == 0) {
        // The re-exec'd victim: say how dumpable the exec left it.
        char c = (char) prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
        int fd = atoi(argv[2]);
        if (write(fd, &c, 1) != 1)
            return 1;
        close(fd);
        for (;;)
            pause();
    }
    test_init(argc, argv);
    // self_path must survive a chdir and a privilege drop.
    static char abs_self[512];
    if (self_path[0] != '/' && realpath(self_path, abs_self) != NULL)
        self_path = abs_self;

    if (geteuid() == 0) {
        // A root process holding a pipe with something in it: the reopen
        // through /proc/<pid>/fd/N used to hand the pipe itself to anyone.
        int secret[2];
        if (pipe(secret) < 0) {
            failf("pipe", (uint64_t) -1, (uint64_t) errno, 0, 0, 0, 0);
            return finish_suite("proc_ptrace_access");
        }
        if (write(secret[1], "hunter2", 7) != 7)
            failf("pipe write", (uint64_t) -1, (uint64_t) errno, 0, 0, 0, 0);
        pid_t root_victim = spawn_victim(setup_plain, NULL);
        close(secret[1]);
        int dropped_ok = -1;
        pid_t dropped = spawn_victim(setup_drop_to_unpriv, &dropped_ok);
        check("setresuid drop leaves the process undumpable", dropped_ok == 0, dropped_ok, 0);

        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            close(secret[0]);
            if (setgroups(0, NULL) < 0 || setgid(UNPRIV_GID) < 0 || setuid(UNPRIV_UID) < 0) {
                printf("FAIL drop privilege errno=%d\n", errno);
                _exit(1);
            }
            if (chdir("/") < 0)
                _exit(1);
            // Dropping from root made this process undumpable, as it does on
            // Linux (commit_creds), and a fork would hand that on to every
            // "own" victim below. Say so, then become an ordinary process of
            // this uid again, which anyone may do for themselves.
            int d = prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
            check("prober undumpable after dropping root", d == 0, d, 0);
            if (prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) < 0)
                failf("PR_SET_DUMPABLE(1)", (uint64_t) -1, (uint64_t) errno, 0, 0, 0, 0);
            d = prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
            check("PR_SET_DUMPABLE(1) takes", d == 1, d, 1);
            prober(root_victim, secret[0], dropped_ok == 0 ? dropped : 0);
            fflush(stdout);
            _exit(failures_total > 0 ? 1 : 0);
        }
        int status;
        if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
            failures_total++;
        close(secret[0]);
        reap(root_victim);
        reap(dropped);
    } else {
        // Unprivileged: init is somebody else's, and nothing else can be set up.
        pid_t init = 1;
        struct stat st;
        if (getpid() == 1 || stat("/proc/1", &st) < 0 || st.st_uid != 0) {
            test_logf("pid 1 is not a root process of someone else's; root legs skipped\n");
            init = 0;
        }
        prober(init, 0, 0);
    }
    return finish_suite("proc_ptrace_access");
}
