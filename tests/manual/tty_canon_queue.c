// What a canonical terminal says is waiting, and what it hands over.
//
//   A typed ^D ends the current line. If the line is empty the read returns
//   0, which is how end-of-input reaches a program reading a terminal -- the
//   ^D that finishes a here-document, or ends input to `cat`, `mail`, `bc`,
//   or anything else reading a line at a time.
//
//   AOK consumed a queued ^D as a side effect of the read BEFORE it. Typing
//   "hi\n^D" delivered "hi\n" and swallowed the EOF, so the next read had
//   nothing to return and nothing coming: it blocked forever, and poll agreed
//   there was nothing there. The cleanup existed for a real reason -- a bare
//   ^D must not return 0 over and over -- but it could not tell the EOF that
//   ended THIS line from one queued behind a completed line.
//
//   FIONREAD reported the raw buffer. Under ICANON a read cannot return
//   anything until a whole line is ready, so a caller that sizes its read
//   from FIONREAD, or uses it to decide whether to read at all, was told
//   about characters the next read would have blocked on. Linux counts the
//   first complete line and skips the EOF delimiter: the number is exactly
//   what a read would hand back.
//
// Measured against x86_64 glibc on Linux 6.12 over a pty.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

static int mfd = -1, sfd = -1;

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-56s got=%-8ld want=%ld\n", label, got, want);
}

static int open_pty(void) {
    mfd = posix_openpt(O_RDWR | O_NOCTTY);
    if (mfd < 0)
        return -1;
    if (grantpt(mfd) != 0 || unlockpt(mfd) != 0)
        return -1;
    char *name = ptsname(mfd);
    if (name == NULL)
        return -1;
    sfd = open(name, O_RDWR | O_NOCTTY);
    if (sfd < 0)
        return -1;
    struct termios t;
    if (tcgetattr(sfd, &t) != 0)
        return -1;
    t.c_lflag |= ICANON;
    t.c_lflag &= ~ECHO;
    return tcsetattr(sfd, TCSANOW, &t);
}

static void close_pty(void) {
    if (mfd >= 0) close(mfd);
    if (sfd >= 0) close(sfd);
    mfd = sfd = -1;
}

static void feed(const char *s, size_t n) {
    ssize_t w = write(mfd, s, n);
    (void) w;
    // The bytes cross a driver; give the line discipline a moment.
    usleep(150000);
}

// A read that must not block: returns the byte count, or -1 if nothing was
// ready within the timeout. Guards the whole point of this test -- an EOF
// that is not delivered shows up as a hang, and a hang has to be reported as
// a failure rather than stalling the suite.
static long read_ready(int fd, char *buf, size_t n, int ms) {
    struct pollfd pf = { fd, POLLIN, 0 };
    int r = poll(&pf, 1, ms);
    if (r <= 0)
        return -1;
    return (long) read(fd, buf, n);
}

// Read everything the terminal will give up, so one section cannot leave
// input behind for the next to trip over.
static void drain(void) {
    char scratch[128];
    struct pollfd pf = { sfd, POLLIN, 0 };
    for (int i = 0; i < 32 && poll(&pf, 1, 200) > 0; i++) {
        if (read(sfd, scratch, sizeof scratch) < 0)
            break;
        pf.revents = 0;
    }
}

static long fionread(void) {
    int n = -1;
    if (ioctl(sfd, FIONREAD, &n) != 0)
        return -1000 - errno;
    return n;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(90));
    // A pty hangup on the way out must not take the test with it.
    signal(SIGHUP, SIG_IGN);

    if (open_pty() != 0) {
        printf("tty_canon_queue: SKIP (no pty available: %s)\n", strerror(errno));
        return 0;
    }

    char buf[64];

    // ---- a completed line, then a queued EOF ----------------------------
    // The case that hung: the EOF belongs to the SECOND read.
    feed("hi\n\004", 4);
    ck("a complete line is readable", read_ready(sfd, buf, sizeof buf, 2000), 3);
    ck("  and it is the line", memcmp(buf, "hi\n", 3) == 0 ? 1 : 0, 1);
    // The queued EOF is still there -- poll must see it, and the read must
    // return 0 rather than blocking.
    {
        struct pollfd pf = { sfd, POLLIN, 0 };
        ck("  poll sees the queued EOF", poll(&pf, 1, 2000) > 0 ? 1 : 0, 1);
    }
    ck("  and reading it gives 0 (EOF)", read_ready(sfd, buf, sizeof buf, 2000), 0);
    // ...and it is gone: a second EOF was never typed, so nothing is ready.
    ck("  the EOF is not delivered twice", read_ready(sfd, buf, sizeof buf, 300), -1);

    // ---- a bare EOF ------------------------------------------------------
    feed("\004", 1);
    ck("a bare EOF reads 0", read_ready(sfd, buf, sizeof buf, 2000), 0);
    ck("  once, not forever", read_ready(sfd, buf, sizeof buf, 300), -1);

    // ---- an EOF that terminates a partial line ---------------------------
    // "abc^D" delivers "abc" with no newline, and the EOF is consumed with
    // it: there is no second read waiting behind it.
    feed("abc\004", 4);
    ck("EOF after a partial line delivers the line", read_ready(sfd, buf, sizeof buf, 2000), 3);
    ck("  and it is the text", memcmp(buf, "abc", 3) == 0 ? 1 : 0, 1);
    ck("  with nothing left behind", read_ready(sfd, buf, sizeof buf, 300), -1);

    // ---- two EOFs in a row ----------------------------------------------
    feed("\004\004", 2);
    ck("the first of two EOFs reads 0", read_ready(sfd, buf, sizeof buf, 2000), 0);
    ck("  and so does the second", read_ready(sfd, buf, sizeof buf, 2000), 0);
    ck("  and then nothing", read_ready(sfd, buf, sizeof buf, 300), -1);

    // ---- FIONREAD counts complete lines ---------------------------------
    ck("FIONREAD on an empty queue", fionread(), 0);
    feed("partial", 7);
    // The bytes are buffered, but a read would block on them.
    ck("FIONREAD with a partial line is 0", fionread(), 0);
    feed("\n", 1);
    ck("  and 8 once the line is complete", fionread(), 8);
    ck("  which is what the read returns", read_ready(sfd, buf, sizeof buf, 2000), 8);
    ck("FIONREAD is 0 again", fionread(), 0);
    // Two complete lines: BOTH count. A read returns one line at a time, but
    // the question FIONREAD answers is how much input is available, and every
    // complete line is. (Measured: Linux reports 8 here, not 4.)
    feed("one\ntwo\n", 8);
    ck("FIONREAD counts every complete line", fionread(), 8);
    ck("  though the read still returns one", read_ready(sfd, buf, sizeof buf, 2000), 4);
    ck("  leaving the second", fionread(), 4);
    ck("  read it", read_ready(sfd, buf, sizeof buf, 2000), 4);
    // A trailing PARTIAL line is not available and does not count.
    feed("one\ntwo", 7);
    ck("a trailing partial line is excluded", fionread(), 4);
    ck("  read the complete one", read_ready(sfd, buf, sizeof buf, 2000), 4);
    ck("  and the partial still counts 0", fionread(), 0);
    feed("\n", 1);
    ck("  until it is finished", fionread(), 4);
    ck("  then read it", read_ready(sfd, buf, sizeof buf, 2000), 4);
    // An EOF among complete lines is a delimiter that yields no byte.
    feed("a\n\004\nb\n", 6);
    ck("EOFs are subtracted from the count", fionread(), 5);
    drain();
    // A bare EOF is a delimiter a read returns nothing for, so it counts 0.
    drain();
    feed("\004", 1);
    ck("FIONREAD for a bare EOF is 0", fionread(), 0);
    ck("  even though a read is ready", read_ready(sfd, buf, sizeof buf, 2000), 0);
    drain();

    // ---- non-canonical FIONREAD still counts raw bytes -------------------
    drain();
    {
        struct termios t;
        tcgetattr(sfd, &t);
        struct termios raw = t;
        cfmakeraw(&raw);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 5;
        tcsetattr(sfd, TCSANOW, &raw);
        feed("partial", 7);
        ck("FIONREAD in raw mode counts every byte", fionread(), 7);
        ck("  and the read returns them", read_ready(sfd, buf, sizeof buf, 2000), 7);

        // VMIN=0 VTIME>0: the read waits for the timer rather than returning
        // at once, which is the neighbouring promise about "nothing is ready".
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        ssize_t n = read(sfd, buf, sizeof buf);
        clock_gettime(CLOCK_MONOTONIC, &b);
        double ms = (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1e6;
        ck("VMIN=0 VTIME=5 read returns 0", (long) n, 0);
        test_logf("  %-56s %.0fms\n", "  after waiting", ms);
        ck("  having waited for the timer", ms >= 300 ? 1 : 0, 1);
        ck("  and not much longer", ms < 3000 ? 1 : 0, 1);
        tcsetattr(sfd, TCSANOW, &t);
    }

    close_pty();
    return finish_suite("tty_canon_queue");
}
