// Four interfaces that reported the wrong shape or the wrong reason.
//
//   An inotify record's name is padded to a multiple of sizeof(struct
//   inotify_event) -- 16 -- not to 4. That keeps every record 16-byte aligned,
//   which is what makes the documented read loop (cast each record in place,
//   step by sizeof + len) legal. Padding to 4 put records at addresses the
//   struct is not aligned for and gave every event a length no reader
//   computing it for itself would predict.
//
//   ptrace answered EPERM where Linux answers ESRCH: for a pid that does not
//   exist, for one that is not our tracee, and for one that is not stopped.
//   EPERM tells a tracer it lacks permission for a process that has simply
//   exited, and debuggers act on that difference -- one retries or reports a
//   permission problem to the user, the other reaps the child and moves on.
//
//   clone3(CLONE_CLEAR_SIGHAND) was accepted and treated as a no-op, on the
//   reasoning that a new task gets a fresh sighand anyway. It does, but a
//   fresh sighand is a COPY of the parent's dispositions, which is the very
//   thing the flag exists to undo. glibc's posix_spawn sets it
//   unconditionally and relies on it entirely -- the spawned child issues no
//   rt_sigaction of its own -- so a child spawned from a process with
//   handlers installed inherited them.
//
//   io_getevents read a 32-bit timespec out of a 64-bit guest's buffer, so
//   tv_sec swallowed both halves and tv_nsec came back 0. Every sub-second
//   timeout became no timeout at all and the call returned immediately: a
//   caller polling an idle context with a 500ms budget got a busy loop.
//
//   A write of at most PIPE_BUF to a pipe or FIFO is ATOMIC -- with
//   insufficient room it writes NOTHING and blocks or returns EAGAIN, rather
//   than putting in what fits. That guarantee is why several processes may
//   share one FIFO for log lines or records: a partial write splits a record
//   and the next writer's bytes land in the middle of it.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/mount.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#ifndef CLONE_CLEAR_SIGHAND
#define CLONE_CLEAR_SIGHAND 0x100000000ULL
#endif

static char base[80];

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-58s got=%-10ld want=%ld\n", label, got, want);
}

static void p(char *o, size_t n, const char *r) { snprintf(o, n, "%s/%s", base, r); }

// ---- inotify record shape --------------------------------------------
static void test_inotify_align(void) {
    char d[128];
    p(d, sizeof d, "ino");
    ck("mkdir a watched directory", mkdir(d, 0755), 0);
    int fd = inotify_init1(IN_NONBLOCK);
    ck("inotify_init1", fd >= 0 ? 1 : 0, 1);
    if (fd < 0)
        return;
    ck("add a watch", inotify_add_watch(fd, d, IN_CREATE) >= 0 ? 1 : 0, 1);
    // 4, 1 and 20 characters: 5, 2 and 21 bytes with the NUL, so 16, 16 and
    // 32 once rounded up.
    const char *names[] = { "file", "a", "twenty-char-name-abc" };
    for (int i = 0; i < 3; i++) {
        char f[224];
        snprintf(f, sizeof f, "%s/%s", d, names[i]);
        int t = open(f, O_WRONLY | O_CREAT, 0644);
        if (t >= 0)
            close(t);
    }
    usleep(250000);
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof buf);
    ck("read the events", n > 0 ? 1 : 0, 1);
    const long want[] = { 16, 16, 32 };
    int i = 0;
    for (ssize_t off = 0; off + (ssize_t) sizeof(struct inotify_event) <= n && i < 3; i++) {
        struct inotify_event *e = (struct inotify_event *) (buf + off);
        char lab[80];
        snprintf(lab, sizeof lab, "  event for a %zu-char name has len", strlen(names[i]));
        ck(lab, (long) e->len, want[i]);
        // The record after this one has to be 16-byte aligned, which is the
        // property the padding exists for.
        snprintf(lab, sizeof lab, "  and the next record is 16-byte aligned");
        ck(lab, (long) ((off + sizeof(struct inotify_event) + e->len) % 16), 0);
        off += sizeof(struct inotify_event) + e->len;
    }
    ck("all three events arrived", i, 3);
    close(fd);
}

// ---- ptrace error codes ------------------------------------------------
static void test_ptrace_esrch(void) {
    errno = 0;
    long r = ptrace(PTRACE_PEEKDATA, 999000, (void *) 0, NULL);
    ck("ptrace on a nonexistent pid is ESRCH", r < 0 ? errno : 0, ESRCH);

    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        alarm(10);
        pause();
        _exit(0);
    }
    usleep(250000);
    errno = 0;
    r = ptrace(PTRACE_PEEKDATA, c, (void *) 0, NULL);
    ck("ptrace on a process we do not trace is ESRCH", r < 0 ? errno : 0, ESRCH);
    errno = 0;
    r = ptrace(PTRACE_CONT, c, NULL, NULL);
    ck("  and so is resuming it", r < 0 ? errno : 0, ESRCH);
    // ...and specifically not EPERM, which is what it used to be.
    ck("  never EPERM", r < 0 && errno == EPERM ? 1 : 0, 0);
    kill(c, SIGKILL);
    int st;
    waitpid(c, &st, 0);
}

// ---- clone3(CLONE_CLEAR_SIGHAND) --------------------------------------
struct clone_args_ {
    unsigned long long flags, pidfd, child_tid, parent_tid, exit_signal;
    unsigned long long stack, stack_size, tls, set_tid, set_tid_size, cgroup;
};
static void onsig(int s) { (void) s; }

static void test_clone_clear_sighand(void) {
#ifdef SYS_clone3
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = onsig;
    ck("install a SIGUSR1 handler", sigaction(SIGUSR1, &sa, NULL), 0);
    // ...and an IGNORE, which SURVIVES: Linux calls flush_signal_handlers
    // with force_default 0, resetting a handler only when it is not SIG_IGN.
    // "Ignored" is an inherited property a child is meant to keep. (Measured;
    // this test first asserted the opposite and the oracle disproved it.)
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    ck("and ignore SIGUSR2", sigaction(SIGUSR2, &sa, NULL), 0);

    struct clone_args_ ca;
    memset(&ca, 0, sizeof ca);
    ca.flags = CLONE_CLEAR_SIGHAND;
    ca.exit_signal = SIGCHLD;
    fflush(NULL);
    errno = 0;
    long pid = syscall(SYS_clone3, &ca, sizeof ca);
    if (pid < 0) {
        failf("clone3 with CLONE_CLEAR_SIGHAND", (uint64_t) errno, 0, 0, 0, 0, 0);
        return;
    }
    if (pid == 0) {
        struct sigaction got1, got2;
        sigaction(SIGUSR1, NULL, &got1);
        sigaction(SIGUSR2, NULL, &got2);
        _exit((got1.sa_handler == SIG_DFL ? 1 : 0) | (got2.sa_handler == SIG_DFL ? 2 : 0));
    }
    int st;
    waitpid((pid_t) pid, &st, 0);
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    ck("the child's handler is reset to SIG_DFL", code & 1, 1);
    ck("  while its IGNORED signal stays ignored", (code & 2) ? 1 : 0, 0);

    // A child cloned WITHOUT the flag still inherits, so the flag is what did
    // it rather than something resetting everything unconditionally.
    memset(&ca, 0, sizeof ca);
    ca.exit_signal = SIGCHLD;
    fflush(NULL);
    pid = syscall(SYS_clone3, &ca, sizeof ca);
    if (pid == 0) {
        struct sigaction got;
        sigaction(SIGUSR1, NULL, &got);
        _exit(got.sa_handler == SIG_DFL ? 0 : 5);
    }
    if (pid > 0) {
        waitpid((pid_t) pid, &st, 0);
        ck("without the flag the handler is inherited",
           WIFEXITED(st) ? WEXITSTATUS(st) : -1, 5);
    }
    // Clearing dispositions shared with the parent would clobber the
    // parent's, so the combination is refused.
    memset(&ca, 0, sizeof ca);
    ca.flags = CLONE_CLEAR_SIGHAND | 0x00000800 /* CLONE_SIGHAND */;
    ca.exit_signal = SIGCHLD;
    errno = 0;
    pid = syscall(SYS_clone3, &ca, sizeof ca);
    ck("CLEAR_SIGHAND with CLONE_SIGHAND is EINVAL", pid < 0 ? errno : 0, EINVAL);
    if (pid == 0)
        _exit(0);
    if (pid > 0)
        waitpid((pid_t) pid, &st, 0);

    signal(SIGUSR1, SIG_DFL);
    signal(SIGUSR2, SIG_DFL);
#endif
}

// ---- io_getevents honours a sub-second timeout ------------------------
static void test_aio_timeout(void) {
#ifdef SYS_io_setup
    unsigned long ctx = 0;
    errno = 0;
    if (syscall(SYS_io_setup, 8, &ctx) != 0) {
        printf("fd_conventions: NOTE io_setup unavailable (%s); skipping aio\n", strerror(errno));
        return;
    }
    // SYS_io_getevents takes the KERNEL's timespec -- old_timespec32 on a
    // 32-bit ABI, __kernel_timespec on a 64-bit one -- which is two longs
    // either way. musl on i386 has 64-bit time_t, so ITS struct timespec is 16
    // bytes with tv_nsec at offset 8, and the kernel read {0,0} out of the two
    // halves of a zero tv_sec: "poll, do not wait". Two longs tracks the ABI
    // automatically. Verified against Linux 6.12, which does the same thing.
    struct { long tv_sec; long tv_nsec; } ts = { 0, 500000000 };
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    errno = 0;
    // min_nr 1 with nothing submitted: the only way out is the timeout.
    long r = syscall(SYS_io_getevents, ctx, 1L, 1L, NULL, &ts);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ms = (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1e6;
    ck("io_getevents times out with 0 events", r, 0);
    test_logf("  %-58s %.0fms\n", "  after waiting", ms);
    ck("  having actually waited", ms >= 300 ? 1 : 0, 1);
    ck("  and not much longer", ms < 3000 ? 1 : 0, 1);
    syscall(SYS_io_destroy, ctx);
#endif
}

// ---- PIPE_BUF atomicity on a FIFO --------------------------------------
static void test_fifo_atomic(void) {
    // A FIFO on a tmpfs, which is the buffer the kernel here owns. (A pipe(2)
    // pair is a HOST pipe: Darwin's own PIPE_BUF is 512, and it has no way to
    // report free space, so the guarantee cannot be imposed on one from here.
    // See docs/TODO.md.)
    char m[128], f[192];
    p(m, sizeof m, "tf");
    ck("mkdir a mount point", mkdir(m, 0755), 0);
    errno = 0;
    if (mount("none", m, "tmpfs", 0, NULL) != 0) {
        printf("fd_conventions: NOTE tmpfs mount refused (%s); skipping the FIFO section\n",
               strerror(errno));
        return;
    }
    snprintf(f, sizeof f, "%s/fifo", m);
    ck("mkfifo", mkfifo(f, 0666), 0);
    int rd = open(f, O_RDONLY | O_NONBLOCK);
    ck("open the read end", rd >= 0 ? 1 : 0, 1);
    int wr = open(f, O_WRONLY | O_NONBLOCK);
    ck("open the write end", wr >= 0 ? 1 : 0, 1);
    if (rd < 0 || wr < 0) {
        umount2(m, 2);
        return;
    }
    // Fill it, then drain a known small amount so a 1024-byte write cannot
    // fit but a 600-byte one can.
    char chunk[4096];
    memset(chunk, 'f', sizeof chunk);
    long filled = 0;
    for (int i = 0; i < 64; i++) {
        ssize_t w = write(wr, chunk, sizeof chunk);
        if (w <= 0)
            break;
        filled += w;
    }
    ck("the fifo filled up", filled > 0 ? 1 : 0, 1);
    char drain[600];
    ck("drain 600 bytes", (long) read(rd, drain, sizeof drain), 600);

    char buf[1024];
    memset(buf, 'x', sizeof buf);
    errno = 0;
    ssize_t w = write(wr, buf, sizeof buf);
    // All or nothing: 1024 <= PIPE_BUF, and only ~600 bytes fit.
    ck("a 1024-byte write that cannot fit writes nothing", w < 0 ? 1 : 0, 1);
    ck("  with EAGAIN", w < 0 ? errno : 0, EAGAIN);
    // A write that DOES fit still goes through, so the rule is a fit check
    // and not a blanket refusal. Drain a whole page first: a pipe frees space
    // a buffer page at a time, so the 600 bytes taken above may have freed
    // nothing at all. (Measured; a 512-byte write after only that drain fails
    // on Linux too.)
    char page[4096];
    ck("drain a full page", (long) read(rd, page, sizeof page) > 0 ? 1 : 0, 1);
    errno = 0;
    w = write(wr, buf, 1024);
    ck("a 1024-byte write that now fits succeeds", (long) w, 1024);
    // A write LARGER than PIPE_BUF may be partial -- the guarantee is bounded.
    char big[8192];
    memset(big, 'y', sizeof big);
    errno = 0;
    w = write(wr, big, sizeof big);
    ck("an 8192-byte write may be short or EAGAIN", w < 0 || w < 8192 ? 1 : 0, 1);

    close(rd);
    close(wr);
    umount2(m, 2 /* MNT_DETACH */);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));
    signal(SIGPIPE, SIG_IGN);
    snprintf(base, sizeof base, "/tmp/fdconv-%d", (int) getpid());
    char cmd[160];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd) < 0)
        failures_total++;
    ck("mkdir base", mkdir(base, 0755), 0);

    test_inotify_align();
    test_ptrace_esrch();
    test_clone_clear_sighand();
    test_aio_timeout();
    if (geteuid() == 0)
        test_fifo_atomic();
    else
        printf("fd_conventions: NOTE not root; skipping the FIFO section\n");

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd) < 0)
        failures_total++;
    return finish_suite("fd_conventions");
}
