// checkpoint_anonfd.c -- descriptors with no file behind them, across a
// checkpoint. Driven by checkpoint_anonfd.sh.
//
// Every one of these used to come back as a copy of the standard input: struct
// fd is zero-initialised, so each had real_fd 0 and the checkpoint took it for
// a standard stream. A restored dbus-daemon got /dev/null where its epoll set
// had been, spun at a full core, and never served the bus again -- and every
// login on the device waits on the bus.
//
// Each check exercises the descriptor, not just its presence: the epoll set
// has to report the pipe that became readable, with the data it was given; the
// inotify watch has to report under the number it was added with.
//
// memfd-shared: a second description of the same memfd, opened read-only
// through /proc/self/fd, comes back as a description of the SAME memfd -- a
// write through the first is read through the second -- each at its own
// position, the second still read-only, and the memfd's size its contents'.
// Each description used to come back as a memfd of its own, and every
// restored memfd said it was empty and was read from its start.
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <poll.h>
#include <errno.h>
#include <unistd.h>

static void check(const char *what, int ok, const char *detail) {
    printf("%s %s%s%s\n", ok ? "OK" : "FAIL", what, detail[0] ? ": " : "", detail);
    fflush(stdout);
}

int main(void) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return 1;
    int ep = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLIN, .data.u64 = 0x1122334455667788ULL};
    epoll_ctl(ep, EPOLL_CTL_ADD, pipefd[0], &ev);

    int efd = eventfd(0, 0);
    uint64_t v = 41;
    write(efd, &v, sizeof(v));

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR2);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sfd = signalfd(-1, &mask, 0);

    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    struct itimerspec its = {.it_value = {0, 200000000}, .it_interval = {0, 200000000}};
    timerfd_settime(tfd, 0, &its, NULL);

    mkdir("/tmp/ckanon", 0755);
    int ifd = inotify_init1(0);
    inotify_add_watch(ifd, "/", IN_CREATE);              // wd 1, to leave a gap
    int wd = inotify_add_watch(ifd, "/tmp/ckanon", IN_CREATE);
    inotify_rm_watch(ifd, 1);

    int mfd = memfd_create("ckanon-memfd", 0);
    write(mfd, "memfd-payload", 13);
    char mpath[64];
    snprintf(mpath, sizeof(mpath), "/proc/self/fd/%d", mfd);
    int mro = open(mpath, O_RDONLY);
    char skip[5];
    read(mro, skip, 5);                                  // mro at 5, mfd at 13

    pid_t child = fork();
    if (child == 0) {
        for (int i = 0; i < 12; i++) sleep(1);
        _exit(7);
    }
    int pfd = (int) syscall(SYS_pidfd_open, child, 0);

    // A pidfd on a process already reaped when the checkpoint is taken: its
    // number may belong to someone else by then, so it must come back naming
    // no process at all -- readable, and ESRCH to signal -- as Linux has it.
    pid_t gone = fork();
    if (gone == 0)
        _exit(0);
    int gfd = (int) syscall(SYS_pidfd_open, gone, 0);
    waitpid(gone, NULL, 0);

    // Everything is set up: say so. The harness checkpoints the moment this
    // file exists (ISH_CHECKPOINT_AFTER=@...) rather than a fixed time after
    // the CLI started, which on a slow boot was before this ran at all. The
    // checkpoint lands in the sleep below, and the script kills this run once
    // the image exists, so everything after it runs only in the restored guest.
    close(open("/realmnt/ready", O_WRONLY | O_CREAT, 0644));
    for (int i = 0; i < 6; i++) sleep(1);

    char d[160];
    // epoll: the pipe becomes readable, and the event carries our data.
    write(pipefd[1], "x", 1);
    struct epoll_event out = {0};
    int n = epoll_wait(ep, &out, 1, 2000);
    snprintf(d, sizeof(d), "n=%d data=%#llx", n, (unsigned long long) out.data.u64);
    check("epoll", n == 1 && out.data.u64 == 0x1122334455667788ULL, d);

    // eventfd: the count written before the checkpoint.
    uint64_t got = 0;
    ssize_t r = read(efd, &got, sizeof(got));
    snprintf(d, sizeof(d), "read=%zd value=%llu", r, (unsigned long long) got);
    check("eventfd", r == 8 && got == 41, d);

    // signalfd: the mask, by receiving the signal it names.
    kill(getpid(), SIGUSR2);
    struct signalfd_siginfo si = {0};
    r = read(sfd, &si, sizeof(si));
    snprintf(d, sizeof(d), "read=%zd signo=%u", r, si.ssi_signo);
    check("signalfd", r == (ssize_t) sizeof(si) && si.ssi_signo == SIGUSR2, d);

    // timerfd: a periodic timer that is still running.
    uint64_t exp = 0;
    r = read(tfd, &exp, sizeof(exp));
    snprintf(d, sizeof(d), "read=%zd expirations=%llu", r, (unsigned long long) exp);
    check("timerfd", r == 8 && exp >= 1, d);

    // inotify: an event, under the watch number it was added with.
    char nb[32];
    snprintf(nb, sizeof(nb), "/tmp/ckanon/f%d", getpid());
    close(open(nb, O_CREAT | O_WRONLY, 0644));
    // Two events are owed: the IN_IGNORED for wd 1, queued by the rm_watch
    // BEFORE the checkpoint and carried across it, and the create, which has
    // to arrive under the number its watch was added with.
    char ibuf[1024];
    struct pollfd ip = {.fd = ifd, .events = POLLIN};
    int pr = poll(&ip, 1, 2000);
    r = pr > 0 ? read(ifd, ibuf, sizeof(ibuf)) : -1;
    int saw_ignored = 0, create_wd = -1;
    for (char *p = ibuf; r > 0 && p < ibuf + r; ) {
        struct inotify_event *ie = (struct inotify_event *) p;
        if (ie->wd == 1 && (ie->mask & IN_IGNORED))
            saw_ignored = 1;
        if (ie->mask & IN_CREATE)
            create_wd = ie->wd;
        p += sizeof(*ie) + ie->len;
    }
    snprintf(d, sizeof(d), "queued IN_IGNORED(wd 1)=%d create wd=%d want=%d",
             saw_ignored, create_wd, wd);
    check("inotify", saw_ignored && create_wd == wd, d);
    unlink(nb);

    // memfd: its contents and its name.
    char mb[32] = {0};
    r = pread(mfd, mb, 13, 0);
    char link[64], lp[64] = {0};
    snprintf(link, sizeof(link), "/proc/self/fd/%d", mfd);
    readlink(link, lp, sizeof(lp) - 1);
    snprintf(d, sizeof(d), "contents=%s link=%s", mb, lp);
    check("memfd", r == 13 && strcmp(mb, "memfd-payload") == 0 &&
                   strstr(lp, "ckanon-memfd") != NULL, d);

    // The read-only second description: the same memfd, its own position.
    struct stat mst = {0};
    fstat(mfd, &mst);
    off_t at_mfd = lseek(mfd, 0, SEEK_CUR), at_mro = lseek(mro, 0, SEEK_CUR);
    int ro_mode = fcntl(mro, F_GETFL) & O_ACCMODE;
    pwrite(mfd, "M", 1, 0);
    char seen = 0;
    ssize_t sr = pread(mro, &seen, 1, 0);
    errno = 0;
    int ro_write = (int) write(mro, "x", 1);
    int ro_errno = errno;
    snprintf(d, sizeof(d), "size=%lld offsets=%lld,%lld mode=%#x shared=%c/%zd write=%d/%d",
             (long long) mst.st_size, (long long) at_mfd, (long long) at_mro, ro_mode,
             seen ? seen : '?', sr, ro_write, ro_errno);
    check("memfd-shared", mro >= 0 && mst.st_size == 13 && at_mfd == 13 && at_mro == 5 &&
                          ro_mode == O_RDONLY && sr == 1 && seen == 'M' &&
                          ro_write < 0 && ro_errno == EBADF, d);

    // pidfd: it names the child, which still exists, and becomes readable
    // when the child exits.
    // Two things /dev/null would get wrong: a pidfd accepts
    // pidfd_send_signal, and it is NOT readable while the child still runs.
    int sig_ok = syscall(SYS_pidfd_send_signal, pfd, 0, NULL, 0) == 0;
    struct pollfd pp = {.fd = pfd, .events = POLLIN};
    int early = poll(&pp, 1, 0);
    pp.revents = 0;
    pr = poll(&pp, 1, 20000);
    int st = 0;
    waitpid(child, &st, 0);
    snprintf(d, sizeof(d), "send_signal=%d readable-while-alive=%d poll=%d exit=%d",
             sig_ok, early, pr, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    check("pidfd", sig_ok && early == 0 && pr == 1 && (pp.revents & POLLIN) &&
                   WIFEXITED(st) && WEXITSTATUS(st) == 7, d);

    struct pollfd gp = {.fd = gfd, .events = POLLIN};
    int gpr = poll(&gp, 1, 0);
    errno = 0;
    long gsig = syscall(SYS_pidfd_send_signal, gfd, 0, NULL, 0);
    int gerr = errno;
    snprintf(d, sizeof(d), "poll=%d revents=%#x send_signal=%ld errno=%d(ESRCH=%d)",
             gpr, gp.revents, gsig, gerr, ESRCH);
    check("pidfd-gone", gpr == 1 && (gp.revents & POLLIN) && gsig == -1 &&
                        gerr == ESRCH, d);
    return 0;
}
