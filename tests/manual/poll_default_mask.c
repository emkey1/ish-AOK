// What poll and select say about files that have no poll operation, and how
// many results they count.
//
//   Darwin's kqueue implements no filter at all for character devices other
//   than ttys (/dev/null, /dev/zero, /dev/random) or for directories, and its
//   poll(2) answers POLLNVAL for them rather than a readiness. AOK passed both
//   of those through: registering such an fd failed the whole guest
//   poll/select/epoll_ctl with EINVAL, and when it did not, the fd was
//   reported permanently not-ready so a blocking wait sat there until its
//   timeout. Linux has no such category -- a file with no ->poll method is
//   polled through DEFAULT_POLLMASK and is *always* readable and writable,
//   whatever its type and whatever access mode it was opened with.
//
//   The EINVAL was not a quiet wrong answer. musl's secure-execution startup
//   polls fds 0, 1 and 2 and calls a_crash() -- a deliberate store to address
//   zero -- if that poll fails, so every setuid or setgid binary died with
//   SIGSEGV before main() whenever any of its three standard fds was a host
//   device node. `sudo something >/dev/null` was a segfault.
//
//   Separately, both calls undercounted. Linux's core_sys_select bumps its
//   return once per descriptor set an fd is reported in, and do_sys_poll once
//   per pollfd entry naming it, so one fd ready for both read and write makes
//   select return 2, and the same fd listed three times makes poll return 3.
//   AOK returned 1 for each: it counted ready fds instead of ready results.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-54s got=%-6ld want=%ld\n", label, got, want);
}

// poll() with an infinite timeout: on Linux every object below is always
// ready, so this returns at once. A wrong "not ready" hangs here rather than
// answering, which is why the timeout is infinite and the watchdog is the
// backstop -- a bounded timeout would turn the hang into a quiet wrong answer.
static int ready_mask(int fd) {
    struct pollfd p = { .fd = fd, .events = POLLIN | POLLOUT | POLLPRI };
    if (poll(&p, 1, -1) < 0)
        return -1;
    return p.revents;
}

static void always_ready(const char *what, int fd) {
    if (fd < 0) {
        test_logf("  %-54s (unavailable: %s)\n", what, strerror(errno));
        return;
    }
    ck(what, ready_mask(fd), POLLIN | POLLOUT);
    close(fd);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    // ---- DEFAULT_POLLMASK: always readable and writable -------------------
    // Not conditional on the access mode: Linux reports POLLOUT on a file
    // opened O_RDONLY and POLLIN on one opened O_WRONLY, because the mask is a
    // property of the file having no ->poll, not of the open.
    {
        int rf = open("/tmp/poll_default_mask.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
        ck("regular file opens", rf >= 0, 1);
        if (rf >= 0) {
            if (write(rf, "x", 1) != 1)
                failures_total++;
            always_ready("regular file O_RDWR is IN|OUT", rf);
        }
        always_ready("regular file O_RDONLY is still IN|OUT",
                     open("/tmp/poll_default_mask.dat", O_RDONLY));
        always_ready("regular file O_WRONLY is still IN|OUT",
                     open("/tmp/poll_default_mask.dat", O_WRONLY));
        unlink("/tmp/poll_default_mask.dat");

        always_ready("/dev/null O_RDWR is IN|OUT", open("/dev/null", O_RDWR));
        always_ready("/dev/null O_RDONLY is still IN|OUT", open("/dev/null", O_RDONLY));
        always_ready("/dev/null O_WRONLY is still IN|OUT", open("/dev/null", O_WRONLY));
        always_ready("/dev/zero is IN|OUT", open("/dev/zero", O_RDWR));
        always_ready("/dev/urandom is IN|OUT", open("/dev/urandom", O_RDONLY));
        always_ready("a directory is IN|OUT", open("/tmp", O_RDONLY | O_DIRECTORY));
    }

    // ---- and select agrees, without failing ------------------------------
    // select is the call musl's AT_SECURE path does not use, but it shares the
    // registration that returned EINVAL, so it is worth its own check.
    {
        int nul = open("/dev/null", O_RDWR);
        ck("/dev/null opens", nul >= 0, 1);
        if (nul >= 0) {
            fd_set rs, ws;
            FD_ZERO(&rs);
            FD_ZERO(&ws);
            FD_SET(nul, &rs);
            FD_SET(nul, &ws);
            errno = 0;
            int n = select(nul + 1, &rs, &ws, NULL, NULL);
            ck("select on /dev/null does not fail", n >= 0, 1);
            ck("  and reports it readable", FD_ISSET(nul, &rs), 1);
            ck("  and writable", FD_ISSET(nul, &ws), 1);
            close(nul);
        }
    }

    // ---- the return value counts results, not file descriptors ------------
    {
        int rf = open("/tmp/poll_default_mask2.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
        int pf[2];
        ck("regular file opens", rf >= 0, 1);
        ck("pipe", pipe(pf), 0);
        if (rf >= 0 && write(pf[1], "x", 1) == 1) {
            struct timeval t0 = { 0, 0 };
            fd_set rs, ws;

            FD_ZERO(&rs);
            FD_ZERO(&ws);
            FD_SET(rf, &rs);
            FD_SET(rf, &ws);
            ck("select: one fd in two sets counts 2", select(rf + 1, &rs, &ws, NULL, &t0), 2);

            // ...while two distinct fds in one set each still count 2, which is
            // what a fix that simply doubled everything would break.
            FD_ZERO(&rs);
            FD_ZERO(&ws);
            FD_SET(pf[0], &rs);
            FD_SET(pf[1], &ws);
            t0 = (struct timeval) { 0, 0 };
            ck("select: two fds in one set each counts 2",
               select((pf[0] > pf[1] ? pf[0] : pf[1]) + 1, &rs, &ws, NULL, &t0), 2);

            FD_ZERO(&rs);
            FD_ZERO(&ws);
            FD_SET(rf, &rs);
            FD_SET(rf, &ws);
            FD_SET(pf[0], &rs);
            t0 = (struct timeval) { 0, 0 };
            ck("select: 2 sets plus another fd counts 3",
               select((rf > pf[0] ? rf : pf[0]) + 1, &rs, &ws, NULL, &t0), 3);

            struct pollfd dup3[3] = {
                { rf, POLLIN, 0 }, { rf, POLLOUT, 0 }, { rf, POLLIN | POLLOUT, 0 },
            };
            ck("poll: one fd in three entries counts 3", poll(dup3, 3, 0), 3);

            struct pollfd two[2] = { { rf, POLLIN, 0 }, { pf[0], POLLIN, 0 } };
            ck("poll: two ready fds count 2", poll(two, 2, 0), 2);

            // An entry whose ready types its own events mask out is not ready
            // and must not be counted: pf[1] is the write end, so POLLIN on it
            // is never satisfied.
            struct pollfd one[2] = { { rf, POLLIN, 0 }, { pf[1], POLLIN, 0 } };
            ck("poll: one ready and one not counts 1", poll(one, 2, 0), 1);
        }
        if (rf >= 0)
            close(rf);
        close(pf[0]);
        close(pf[1]);
        unlink("/tmp/poll_default_mask2.dat");
    }

    return finish_suite("poll_default_mask");
}
