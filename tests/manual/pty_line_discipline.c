// Covers a set of related pty/tty line-discipline gaps (the first three from
// issue #423 Tier 3):
//  1. ECHOKE vs plain ECHOK on VKILL -- and the fact that ECHOKE_ was
//     previously defined at the wrong termios c_lflag bit (1<<6, which is
//     actually ECHONL's position) so it never matched what any real program
//     set via tcsetattr.
//  2. tty_poll() unconditionally reported POLLOUT even when the peer side of
//     a pty had a full input buffer and a write would actually block/EAGAIN.
//  3. A freshly allocated pty must start with packet mode OFF. tty_alloc()
//     malloc'd struct tty without zeroing it and no init hook ever set
//     pty.packet_mode, so a recycled block whose previous owner had turned
//     packet mode on (TIOCPKT) handed the new pty a stale "on": every master
//     read then carried a leading TIOCPKT_DATA status byte (0x00) that the
//     opener never asked for and does not expect.
//  4. A fresh tty's c_cflag was 0, which decodes as B0/CS5/no-CREAD -- see
//     test_default_cflag() below for how that broke ssh to a BSD server.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include "test_common.h"

// TIOCGPKT/TIOCSPTLCK are Linux 3.8+; some older headers lack them.
#ifndef TIOCGPKT
#define TIOCGPKT 0x80045438
#endif

static void dump_buf(char *out, const char *buf, ssize_t n) {
    size_t o = 0;
    for (ssize_t i = 0; i < n; i++) {
        unsigned char c = buf[i];
        if (c == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c < 32 || c == 127) { out[o++] = '^'; out[o++] = c ^ 0x40; }
        else out[o++] = c;
    }
    out[o] = '\0';
}

static void test_kill_echo(const char *name, int echoke_on, const char *expected) {
    int master, slave;
    if (openpty(&master, &slave, NULL, NULL, NULL) != 0) {
        printf("FAIL %s: openpty failed\n", name);
        failures_total++;
        return;
    }
    struct termios t;
    tcgetattr(slave, &t);
    t.c_lflag |= ECHO | ECHOK | ICANON;
    if (echoke_on)
        t.c_lflag |= ECHOKE;
    else
        t.c_lflag &= ~ECHOKE;
    tcsetattr(slave, TCSANOW, &t);

    char kill_char = t.c_cc[VKILL];
    if (write(master, "abc", 3) != 3 || write(master, &kill_char, 1) != 1) {
        printf("FAIL %s: write failed\n", name);
        failures_total++;
        close(master); close(slave);
        return;
    }
    usleep(100000);

    char buf[256];
    ssize_t n = read(master, buf, sizeof(buf));
    char got[512];
    dump_buf(got, buf, n < 0 ? 0 : n);

    test_logf("%s: got=\"%s\"\n", name, got);
    if (strcmp(got, expected) != 0) {
        printf("FAIL %s: got=\"%s\" expected=\"%s\"\n", name, got, expected);
        failures_total++;
    }
    close(master);
    close(slave);
}

// A fresh tty must come up with the same c_cflag Linux's tty_std_termios uses
// (B38400|CS8|CREAD|HUPCL), not zero. Zero decodes as B0 -- "hang up the
// line" -- and cfgetospeed() reads straight out of CBAUD in c_cflag, so a
// zeroed cflag made ssh(1) advertise "ospeed 0" in its pty-req. A BSD sshd
// honours that literally: OpenBSD's TIOCSETA path SIGHUPs the tty's session
// leader whenever c_ospeed is 0, so the remote login shell was killed the
// moment it configured its own tty for line editing -- login appeared to
// succeed, printed the motd, then dropped with exit status 129 (128+SIGHUP).
// Zero also decodes as CS5 and clears CREAD, which ssh forwards as well.
static void test_default_cflag(void) {
    int master, slave;
    if (openpty(&master, &slave, NULL, NULL, NULL) != 0) {
        printf("FAIL default_cflag: openpty failed\n");
        failures_total++;
        return;
    }
    struct termios t;
    if (tcgetattr(slave, &t) != 0) {
        printf("FAIL default_cflag: tcgetattr: %s\n", strerror(errno));
        failures_total++;
        close(master); close(slave);
        return;
    }
    speed_t ospeed = cfgetospeed(&t);
    int csize = t.c_cflag & CSIZE;
    int cread = (t.c_cflag & CREAD) != 0;
    test_logf("default_cflag: c_cflag=0%o ospeed=0%o csize=0%o cread=%d\n",
              (unsigned) t.c_cflag, (unsigned) ospeed, (unsigned) csize, cread);

    if (ospeed == B0) {
        printf("FAIL default_cflag: cfgetospeed()==B0, want a real baud rate "
               "(c_cflag=0%o)\n", (unsigned) t.c_cflag);
        failures_total++;
    }
    if (csize != CS8) {
        printf("FAIL default_cflag: CSIZE=0%o, want CS8=0%o\n",
               (unsigned) csize, (unsigned) CS8);
        failures_total++;
    }
    if (!cread) {
        printf("FAIL default_cflag: CREAD clear, want set\n");
        failures_total++;
    }
    close(master);
    close(slave);
}

static int pollout_ready(int fd) {
    struct pollfd pfd = {.fd = fd, .events = POLLOUT};
    if (poll(&pfd, 1, 0) < 0)
        return -1;
    return (pfd.revents & POLLOUT) ? 1 : 0;
}

static void test_pty_pollout_backpressure(void) {
    int master, slave;
    if (openpty(&master, &slave, NULL, NULL, NULL) != 0) {
        printf("FAIL pty_pollout_backpressure: openpty failed\n");
        failures_total++;
        return;
    }
    struct termios t;
    tcgetattr(slave, &t);
    cfmakeraw(&t);
    tcsetattr(slave, TCSANOW, &t);
    fcntl(master, F_SETFL, O_NONBLOCK);

    int initial = pollout_ready(master);

    char chunk[4096];
    memset(chunk, 'x', sizeof(chunk));
    ssize_t total = 0;
    for (int i = 0; i < 20; i++) {
        ssize_t n = write(master, chunk, sizeof(chunk));
        if (n < 0)
            break;
        total += n;
    }
    int full = pollout_ready(master);

    char drain[8192];
    read(slave, drain, sizeof(drain));
    usleep(50000);
    int after_drain = pollout_ready(master);

    test_logf("pty_pollout_backpressure: initial=%d full=%d(after %zd bytes) after_drain=%d\n",
              initial, full, total, after_drain);

    if (initial != 1 || full != 0 || after_drain != 1) {
        printf("FAIL pty_pollout_backpressure: initial=%d full=%d after_drain=%d (want 1,0,1)\n",
               initial, full, after_drain);
        failures_total++;
    }
    close(master);
    close(slave);
}

// Read whatever the master has, non-blocking, after giving the echo a moment
// to land. Returns the byte count (0 on EAGAIN).
static ssize_t master_slurp(int master, char *buf, size_t cap) {
    usleep(50000);
    int flags = fcntl(master, F_GETFL);
    fcntl(master, F_SETFL, flags | O_NONBLOCK);
    ssize_t n = read(master, buf, cap);
    fcntl(master, F_SETFL, flags);
    return n < 0 ? 0 : n;
}

// A pty is created with packet mode off, and a previous pty that turned it on
// must not be able to leave it on for the next one. The "previous" pty is
// closed first so its kernel-side state is freed (and, on AOK, its struct tty
// block recycled) before the fresh one is opened.
static void test_fresh_pty_packet_mode(void) {
    int master, slave;
    if (openpty(&master, &slave, NULL, NULL, NULL) != 0) {
        printf("FAIL fresh_pty_packet_mode: openpty (first) failed\n");
        failures_total++;
        return;
    }
    int on = 1;
    if (ioctl(master, TIOCPKT, &on) < 0) {
        printf("FAIL fresh_pty_packet_mode: TIOCPKT on: %s\n", strerror(errno));
        failures_total++;
        close(master); close(slave);
        return;
    }
    // Positive control: with packet mode explicitly on, the master read really
    // does carry the leading status byte. Without this a build that ignored
    // TIOCPKT entirely would sail through the rest of the test.
    char buf[256];
    write(master, "abc", 3);
    ssize_t n = master_slurp(master, buf, sizeof(buf));
    int pkt_prefixed = (n == 4 && buf[0] == '\0' && memcmp(buf + 1, "abc", 3) == 0);
    test_logf("fresh_pty_packet_mode: packet-on read n=%zd prefixed=%d\n", n, pkt_prefixed);
    if (!pkt_prefixed) {
        printf("FAIL fresh_pty_packet_mode: packet mode on: got n=%zd, want 4 bytes \"\\0abc\"\n", n);
        failures_total++;
    }
    close(master);
    close(slave);

    // Now a brand new pty: packet mode must be off, and the echo must come
    // back with no status byte in front of it.
    for (int i = 0; i < 4; i++) {
        if (openpty(&master, &slave, NULL, NULL, NULL) != 0) {
            printf("FAIL fresh_pty_packet_mode: openpty (fresh #%d) failed\n", i);
            failures_total++;
            return;
        }
        int pkt = -1;
        if (ioctl(master, TIOCGPKT, &pkt) < 0) {
            // TIOCGPKT is not universally available; the read check below is
            // the one that actually matters.
            test_logf("fresh_pty_packet_mode: TIOCGPKT unavailable: %s\n", strerror(errno));
            pkt = 0;
        }
        write(master, "abc", 3);
        n = master_slurp(master, buf, sizeof(buf));
        int clean = (n == 3 && memcmp(buf, "abc", 3) == 0);
        test_logf("fresh_pty_packet_mode: fresh #%d TIOCGPKT=%d read n=%zd clean=%d\n",
                  i, pkt, n, clean);
        if (pkt != 0) {
            printf("FAIL fresh_pty_packet_mode: fresh pty #%d reports packet mode %d, want 0\n", i, pkt);
            failures_total++;
        }
        if (!clean) {
            printf("FAIL fresh_pty_packet_mode: fresh pty #%d read n=%zd first=0x%02x, want 3 bytes \"abc\"\n",
                   i, n, n > 0 ? (unsigned char) buf[0] : 0);
            failures_total++;
        }
        close(master);
        close(slave);
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_kill_echo("kill_with_echoke", 1, "abc^H ^H^H ^H^H ^H");
    test_kill_echo("kill_without_echoke", 0, "abc^U\\r\\n");
    test_default_cflag();
    test_pty_pollout_backpressure();
    test_fresh_pty_packet_mode();
    return finish_suite("pty_line_discipline");
}
