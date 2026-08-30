// Line-discipline editing and the non-canonical read timer.
//
// Three things AOK did not implement, all of them things a person typing at a
// shell without readline hits directly:
//
//   ^W (VWERASE) erases the previous word. It was dropped on the floor, so the
//   word stayed and the ^W vanished.
//
//   ^V (VLNEXT) quotes the next character. Not implemented in either
//   direction: the ^V itself reached the reader as data, AND the character it
//   was supposed to be quoting kept its special meaning -- so `^V ^C` sent an
//   interrupt and delivered a stray 0x16.
//
//   VMIN=0 with VTIME>0 is a read-with-timeout: wait up to VTIME tenths for
//   the first byte, then return what arrived. The wait loop is bounded by
//   VMIN, so at VMIN=0 it never ran and the read came back instantly with
//   nothing -- the VMIN=0 VTIME=0 (pure poll) behaviour instead.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pty.h>
#include <termios.h>
#include <time.h>

#include "test_common.h"

static int m, s;

static void check(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-44s got=%ld want=%ld\n", label, got, want);
}
static int slave_read(char *buf, size_t cap, long ms) {
    struct pollfd p = { s, POLLIN, 0 };
    int total = 0;
    while (total < (int) cap - 1) {
        if (poll(&p, 1, (int) ms) <= 0)
            break;
        ssize_t n = read(s, buf + total, cap - 1 - total);
        if (n <= 0)
            break;
        total += n;
        ms = 60;                    // brief grace for the rest of the line
    }
    buf[total] = '\0';
    return total;
}
static void mwrite(const char *str) { if (write(m, str, strlen(str)) < 0) {} }
static void set_canon(int on) {
    struct termios t;
    tcgetattr(s, &t);
    if (on) t.c_lflag |= (ICANON | ECHO);
    else t.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(s, TCSANOW, &t);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    if (openpty(&m, &s, NULL, NULL, NULL) < 0) {
        printf("tty_line_discipline: SKIP (openpty: %s)\n", strerror(errno));
        return 0;
    }
    long scale = (long) test_watchdog_secs(1);

    // ^W erases the previous word, leaving earlier words alone.
    set_canon(1);
    mwrite("hello world\027there\n");           // \027 == ^W
    {
        char buf[128];
        slave_read(buf, sizeof buf, 400 * scale);
        int ok = strcmp(buf, "hello there\n") == 0;
        if (!ok)
            printf("FAIL ^W: got \"%s\" want \"hello there\\n\"\n", buf);
        check("VWERASE (^W) erases the last word", ok, 1);
    }

    // ^V quotes the next character: a literal ^C reaches the reader as data
    // instead of raising SIGINT.
    {
        char buf[128];
        mwrite("a\026\003b\n");                 // a ^V ^C b \n
        int n = slave_read(buf, sizeof buf, 400 * scale);
        int ok = n == 4 && buf[0] == 'a' && buf[1] == 3 && buf[2] == 'b' && buf[3] == '\n';
        if (!ok) {
            printf("FAIL ^V: got %d bytes:", n);
            for (int i = 0; i < n; i++) printf(" %02x", (unsigned char) buf[i]);
            printf("\n");
        }
        check("VLNEXT (^V) quotes the next character", ok, 1);
    }

    // ^D on an empty line is EOF, and must not swallow the following line.
    {
        char buf[128];
        mwrite("line1\n");
        slave_read(buf, sizeof buf, 400 * scale);
        mwrite("\004");
        struct pollfd p = { s, POLLIN, 0 };
        ssize_t n = 0;
        if (poll(&p, 1, (int) (400 * scale)) > 0) {
            char b2[64];
            n = read(s, b2, sizeof b2);
        }
        check("^D on an empty line reads 0 (EOF)", (long) n, 0);
        mwrite("line2\n");
        int n2 = slave_read(buf, sizeof buf, 500 * scale);
        check("the next line still arrives after EOF",
              n2 > 0 && strcmp(buf, "line2\n") == 0, 1);
    }

    // VMIN=0 VTIME>0: waits up to VTIME tenths, then returns 0.
    {
        set_canon(0);
        struct termios t;
        tcgetattr(s, &t);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 3;                      // 300 ms
        tcsetattr(s, TCSANOW, &t);
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        char c;
        ssize_t n = read(s, &c, 1);
        clock_gettime(CLOCK_MONOTONIC, &b);
        double ms = (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1e6;
        test_logf("  VMIN=0 VTIME=3 returned %zd after %.0fms\n", n, ms);
        check("VMIN=0 VTIME>0 returns 0", (long) n, 0);
        // Generous upper bound: this runs inside the loaded suite.
        check("  after roughly VTIME, not instantly", ms > 150 && ms < 3000, 1);
        set_canon(1);
    }

    close(m);
    close(s);
    return finish_suite("tty_line_discipline");
}
