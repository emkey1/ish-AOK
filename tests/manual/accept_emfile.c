// accept_emfile.c -- accept(2) and socketpair(2) at the descriptor limit.
//
// An AF_UNIX accept whose new descriptor could not be installed (EMFILE: the
// process at RLIMIT_NOFILE) went on to set up the AF_LOCAL peer through f_get()
// of the negative errno -- NULL -- and the write through it aborted the whole
// app. Seen on an A10X iPad when Wayfire, at its descriptor limit, accepted a
// client. Linux returns EMFILE and the pending connection stays queued.
//
// Each case: fill the table up to a small RLIMIT_NOFILE, then the call must
// fail with EMFILE; after one descriptor is freed, the same call must succeed.
// Linux reserves the descriptor before taking the connection, so the client
// is still queued after the EMFILE; taking it first and then failing to
// install it closed it, and the client was dropped.
//
// Output: one "accept_emfile_<case>: PASS" or ": FAIL <why>" line per case,
// then "accept_emfile: PASS" (or FAIL) for the suite.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int failures;

static void report(const char *name, int ok, const char *why) {
    if (ok)
        printf("accept_emfile_%s: PASS\n", name);
    else {
        printf("accept_emfile_%s: FAIL %s\n", name, why);
        failures++;
    }
}

// Open /dev/null until the table is full; returns how many were opened.
static int fill(int *fds, int max) {
    int n = 0;
    while (n < max) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd < 0)
            break;
        fds[n++] = fd;
    }
    return n;
}

int main(void) {
    char path[64];
    snprintf(path, sizeof(path), "/tmp/accept_emfile.%d", (int) getpid());
    unlink(path);

    int lsn = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    if (lsn < 0 || bind(lsn, (struct sockaddr *) &sa, sizeof(sa)) != 0 || listen(lsn, 4) != 0) {
        printf("accept_emfile_setup: FAIL listener: %s\n", strerror(errno));
        return 1;
    }
    int cli = socket(AF_UNIX, SOCK_STREAM, 0);
    if (cli < 0 || connect(cli, (struct sockaddr *) &sa, sizeof(sa)) != 0) {
        printf("accept_emfile_setup: FAIL connect: %s\n", strerror(errno));
        return 1;
    }

    struct rlimit rl = { 64, 64 };
    if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
        printf("accept_emfile_setup: FAIL setrlimit: %s\n", strerror(errno));
        return 1;
    }
    static int fds[64];
    int n = fill(fds, 64);
    // Positive control: the table really is full.
    errno = 0;
    int probe = open("/dev/null", O_RDONLY);
    report("table_full", probe < 0 && errno == EMFILE, "open did not fail with EMFILE");

    errno = 0;
    int got = accept(lsn, NULL, NULL);
    int err = errno;
    char why[128];
    snprintf(why, sizeof(why), "accept returned %d errno %d (%s)", got, err, strerror(err));
    report("accept_at_limit", got < 0 && err == EMFILE, why);

    errno = 0;
    int sv[2] = { -1, -1 };
    int sp = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    err = errno;
    snprintf(why, sizeof(why), "socketpair returned %d errno %d (%s)", sp, err, strerror(err));
    report("socketpair_at_limit", sp < 0 && err == EMFILE, why);

    // One free slot: socketpair needs two, so it must still fail cleanly, and
    // the descriptor it managed to make must not be left behind.
    close(fds[--n]);
    errno = 0;
    sp = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    err = errno;
    int again = open("/dev/null", O_RDONLY);
    snprintf(why, sizeof(why), "socketpair %d errno %d; the freed slot %s",
             sp, err, again >= 0 ? "is free again" : "was leaked");
    report("socketpair_one_slot", sp < 0 && err == EMFILE && again >= 0, why);
    if (again >= 0)
        close(again);

    // The queued connection survived the failed accept. Polled first: a
    // connection the failed accept took and dropped would leave a blocking
    // accept here waiting forever.
    struct pollfd pfd = { .fd = lsn, .events = POLLIN };
    if (poll(&pfd, 1, 5000) != 1) {
        report("accept_after_free", 0, "the queued connection is gone (lost by the failed accept)");
        unlink(path);
        printf("accept_emfile: FAIL connection lost\n");
        return 1;
    }
    got = accept(lsn, NULL, NULL);
    snprintf(why, sizeof(why), "accept after freeing a slot: %d (%s)", got, got < 0 ? strerror(errno) : "ok");
    int ok = got >= 0;
    if (ok) {
        ok = write(cli, "x", 1) == 1;
        char c = 0;
        ok = ok && read(got, &c, 1) == 1 && c == 'x';
        if (!ok)
            snprintf(why, sizeof(why), "accepted, but the connection does not carry data");
    }
    report("accept_after_free", ok, why);

    unlink(path);
    if (failures)
        printf("accept_emfile: FAIL %d case(s)\n", failures);
    else
        printf("accept_emfile: PASS\n");
    return failures != 0;
}
