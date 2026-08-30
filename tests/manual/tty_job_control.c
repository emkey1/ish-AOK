// Job control and XON/XOFF flow control: four things AOK's terminals did not do.
//
//   IXON was never implemented. ^S and ^Q were delivered to the reading
//   program as literal 0x13/0x11 and stopped nothing, so someone pressing ^S
//   to pause a scrolling listing got two junk bytes into whatever was reading
//   and no pause. Linux keeps two separate flags here and the difference is
//   visible: TCOON restarts only output that TCOOFF stopped, so it will NOT
//   clear a ^S.
//
//   tcsetpgrp validated nothing -- the source said so in a TODO -- and stored
//   whatever number it was handed, so a terminal could be pointed at a process
//   group that does not exist or belongs to another session. That value
//   decides who owns the keyboard.
//
//   SIGTTOU was never generated. A background process could write over the
//   foreground job's screen with TOSTOP set, and could change the terminal's
//   settings underneath it. The full check has three exceptions Linux applies
//   in order (ignored/blocked signal, orphaned group, otherwise stop), and AOK
//   had a partial version of it on the read path only.
//
//   A session leader's death did not free its controlling terminal while any
//   other process group in the session survived, and nothing else could ever
//   free it either -- the pts stayed unusable as a controlling terminal for
//   the life of the emulator.
//
// Every expectation measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>

#include "test_common.h"

static int RC, ER;
// errno is read after the call completes, never as a sibling argument --
// argument evaluation order is unspecified and gcc really does read it first.
#define DO(call) do { errno = 0; RC = (call); ER = errno; } while (0)

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-52s got=%-5ld want=%ld\n", label, got, want);
}
static void ck_err(const char *label, int rc, int er, int want_rc, int want_errno) {
    int ok = rc == want_rc && (rc >= 0 || er == want_errno);
    if (!ok)
        failf(label, (uint64_t) rc, (uint64_t) er, 0,
              (uint64_t) want_rc, (uint64_t) want_errno, 0);
    test_logf("  %-52s rc=%-3d errno=%-2d want rc=%-3d errno=%d\n",
              label, rc, rc < 0 ? er : 0, want_rc, want_errno);
}

#define IN_CHILD(...) do {                                                    \
        fflush(NULL);                                                          \
        pid_t c_ = fork();                                                     \
        if (c_ == 0) {                                                         \
            failures_total = 0;   /* only our own, or one failure doubles */   \
            __VA_ARGS__;                                                       \
            fflush(NULL);                                                      \
            _exit(failures_total > 250 ? 250 : (int) failures_total);          \
        }                                                                      \
        int st_;                                                               \
        if (waitpid(c_, &st_, 0) != c_) { failures_total++; break; }           \
        if (WIFSIGNALED(st_)) {                                                \
            printf("FAIL child died on signal %d\n", WTERMSIG(st_));           \
            failures_total++;                                                  \
        } else                                                                 \
            failures_total += (unsigned) WEXITSTATUS(st_);                     \
    } while (0)

static int m, s;

static void open_pty(int canon, int ixon, int ixany) {
    if (openpty(&m, &s, NULL, NULL, NULL) < 0) {
        perror("openpty");
        _exit(2);
    }
    struct termios t;
    tcgetattr(s, &t);
    t.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL);
    if (canon) t.c_lflag |= ICANON | IEXTEN; else t.c_lflag &= ~ICANON;
    if (ixon)  t.c_iflag |= IXON;  else t.c_iflag &= ~IXON;
    if (ixany) t.c_iflag |= IXANY; else t.c_iflag &= ~IXANY;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 3;
    tcsetattr(s, TCSANOW, &t);
}
static void close_pty(void) { close(m); close(s); }

// What the slave read, as a hex string, so a mismatch prints usefully.
static void slave_bytes(char *out, size_t cap) {
    usleep(300000);
    char buf[64];
    struct pollfd p = { s, POLLIN, 0 };
    int n = poll(&p, 1, 900) > 0 ? (int) read(s, buf, sizeof buf) : 0;
    out[0] = '\0';
    for (int i = 0; i < n; i++)
        snprintf(out + strlen(out), cap - strlen(out), "%s%02x",
                 i ? " " : "", (unsigned char) buf[i]);
}
// Is output flowing? A nonblocking write is the cheap, non-hanging way to ask.
static int flowing(void) {
    int fl = fcntl(s, F_GETFL);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    errno = 0;
    ssize_t r = write(s, "hello", 5);
    int e = errno;
    fcntl(s, F_SETFL, fl);
    if (r == 5)
        return 1;
    if (r < 0 && e == EAGAIN)
        return 0;
    printf("FAIL unexpected write result %zd errno=%d\n", r, e);
    failures_total++;
    return -1;
}
static void ck_flow(const char *label, int want) {
    int got = flowing();
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-52s %s (want %s)\n", label,
              got ? "flowing" : "stopped", want ? "flowing" : "stopped");
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(180));

    // ---- IXON: ^S and ^Q are the line discipline's, not the reader's ------
    {
        char hex[128];
        open_pty(1, 1, 0);
        write(m, "a\023\021b\r", 5);             // a ^S ^Q b CR
        slave_bytes(hex, sizeof hex);
        ck("IXON consumes ^S and ^Q", strcmp(hex, "61 62 0a") == 0, 1);
        test_log_if(strcmp(hex, "61 62 0a") != 0, "    got [%s] want [61 62 0a]\n", hex);
        close_pty();

        open_pty(1, 0, 0);
        write(m, "a\023\021b\r", 5);
        slave_bytes(hex, sizeof hex);
        ck("with IXON off they are ordinary data",
           strcmp(hex, "61 13 11 62 0a") == 0, 1);
        test_log_if(strcmp(hex, "61 13 11 62 0a") != 0,
                    "    got [%s] want [61 13 11 62 0a]\n", hex);
        close_pty();

        // ^V quotes a ^S: lnext wins, and output keeps flowing.
        open_pty(1, 1, 0);
        write(m, "a\026\023b\r", 5);             // a ^V ^S b CR
        slave_bytes(hex, sizeof hex);
        ck("^V quotes ^S into data", strcmp(hex, "61 13 62 0a") == 0, 1);
        test_log_if(strcmp(hex, "61 13 62 0a") != 0,
                    "    got [%s] want [61 13 62 0a]\n", hex);
        ck_flow("  and output was never stopped", 1);
        close_pty();

        // flow control is an iflag, so it applies in raw mode too
        open_pty(0, 1, 0);
        write(m, "a\023b", 3);
        slave_bytes(hex, sizeof hex);
        ck("raw mode consumes them as well", strcmp(hex, "61 62") == 0, 1);
        test_log_if(strcmp(hex, "61 62") != 0, "    got [%s] want [61 62]\n", hex);
        close_pty();
    }

    // ---- IXON: ^S really stops output ------------------------------------
    {
        open_pty(0, 1, 0);
        ck_flow("before any ^S", 1);
        write(m, "\023", 1); usleep(250000);
        ck_flow("^S stops output", 0);
        write(m, "\021", 1); usleep(250000);
        ck_flow("^Q starts it again", 1);
        close_pty();

        // TCOON restarts only output TCOOFF stopped -- never a ^S.
        open_pty(0, 1, 0);
        write(m, "\023", 1); usleep(250000);
        tcflow(s, TCOON); usleep(250000);
        ck_flow("tcflow(TCOON) does not clear a ^S", 0);
        close_pty();

        open_pty(0, 1, 0);
        tcflow(s, TCOOFF); usleep(250000);
        ck_flow("tcflow(TCOOFF) stops output", 0);
        tcflow(s, TCOON); usleep(250000);
        ck_flow("  and tcflow(TCOON) releases its own stop", 1);
        close_pty();

        // IXANY: any character resumes, and is still delivered as data.
        open_pty(0, 1, 1);
        write(m, "\023", 1); usleep(250000);
        write(m, "z", 1);    usleep(250000);
        ck_flow("IXANY resumes on any character", 1);
        close_pty();

        // Turning IXON off releases a ^S: no ^Q could ever arrive to do it.
        open_pty(0, 1, 0);
        write(m, "\023", 1); usleep(250000);
        {
            struct termios t;
            tcgetattr(s, &t);
            t.c_iflag &= ~IXON;
            tcsetattr(s, TCSANOW, &t);
        }
        usleep(250000);
        ck_flow("clearing IXON releases a ^S stop", 1);
        close_pty();
    }

    // ---- tcsetpgrp validates what it is given -----------------------------
    IN_CHILD({
        setsid();
        open_pty(0, 1, 0);
        if (ioctl(s, TIOCSCTTY, 0) < 0) {
            printf("FAIL could not take a controlling terminal: %s\n", strerror(errno));
            failures_total++;
        } else {
            pid_t mine = getpgrp();
            struct { const char *n; pid_t g; int e; } cases[] = {
                { "tcsetpgrp: a nonexistent process group", 99999, ESRCH },
                { "tcsetpgrp: a group in another session", 1, EPERM },
                { "tcsetpgrp: a negative pgid", -3, EINVAL },
            };
            for (unsigned i = 0; i < 3; i++) {
                DO(tcsetpgrp(s, cases[i].g));
                ck_err(cases[i].n, RC, ER, -1, cases[i].e);
                ck("  foreground group is left alone", tcgetpgrp(s), mine);
            }
            // ...and the legitimate handover a job-control shell actually does
            fflush(NULL);
            pid_t job = fork();
            if (job == 0) { setpgid(0, 0); pause(); _exit(0); }
            usleep(200000);
            setpgid(job, job);
            DO(tcsetpgrp(s, job));
            ck_err("tcsetpgrp: handing the terminal to a child's group", RC, ER, 0, 0);
            ck("  the foreground group followed", tcgetpgrp(s), job);
            // Taking it back makes us a background process, so SIGTTOU has to
            // be out of the way first -- which is exactly what a shell does.
            signal(SIGTTOU, SIG_IGN);
            DO(tcsetpgrp(s, getpgrp()));
            ck_err("tcsetpgrp: taking it back with SIGTTOU ignored", RC, ER, 0, 0);
            ck("  the foreground group followed back", tcgetpgrp(s), getpgrp());
            kill(job, SIGKILL);
            int st;
            waitpid(job, &st, 0);
        }
    });

    // ---- SIGTTOU: background access to the terminal ------------------------
    IN_CHILD({
        setsid();
        open_pty(0, 1, 0);
        if (ioctl(s, TIOCSCTTY, 0) < 0) {
            printf("FAIL could not take a controlling terminal: %s\n", strerror(errno));
            failures_total++;
        } else {
            struct termios t;
            tcgetattr(s, &t);
            t.c_lflag |= TOSTOP;
            tcsetattr(s, TCSANOW, &t);
            tcsetpgrp(s, getpgrp());
            int st;

            // A background write with TOSTOP set stops the writer. Its parent
            // (us) is in the same session and a different group, so the group
            // is not orphaned and the stop is the right answer.
            fflush(NULL);
            pid_t w = fork();
            if (w == 0) { setpgid(0, 0); write(s, "x", 1); _exit(42); }
            waitpid(w, &st, WUNTRACED);
            ck("a background write with TOSTOP is stopped",
               WIFSTOPPED(st) && WSTOPSIG(st) == SIGTTOU, 1);
            kill(w, SIGKILL); kill(w, SIGCONT); waitpid(w, &st, 0);

            // With TOSTOP clear, the same write is allowed.
            tcgetattr(s, &t);
            t.c_lflag &= ~TOSTOP;
            tcsetattr(s, TCSANOW, &t);
            fflush(NULL);
            w = fork();
            if (w == 0) { setpgid(0, 0); _exit(write(s, "x", 1) == 1 ? 42 : 1); }
            waitpid(w, &st, WUNTRACED);
            ck("  without TOSTOP it is allowed",
               WIFEXITED(st) && WEXITSTATUS(st) == 42, 1);

            // A terminal-MODIFYING ioctl is refused regardless of TOSTOP.
            fflush(NULL);
            pid_t i2 = fork();
            if (i2 == 0) {
                setpgid(0, 0);
                struct termios t2;
                tcgetattr(s, &t2);
                tcsetattr(s, TCSANOW, &t2);
                _exit(43);
            }
            waitpid(i2, &st, WUNTRACED);
            ck("a background tcsetattr is stopped, TOSTOP or not",
               WIFSTOPPED(st) && WSTOPSIG(st) == SIGTTOU, 1);
            kill(i2, SIGKILL); kill(i2, SIGCONT); waitpid(i2, &st, 0);

            // Ignoring SIGTTOU is permission to proceed.
            fflush(NULL);
            pid_t i3 = fork();
            if (i3 == 0) {
                setpgid(0, 0);
                signal(SIGTTOU, SIG_IGN);
                struct termios t2;
                tcgetattr(s, &t2);
                _exit(tcsetattr(s, TCSANOW, &t2) == 0 ? 44 : 1);
            }
            waitpid(i3, &st, WUNTRACED);
            ck("  unless SIGTTOU is ignored, which allows it",
               WIFEXITED(st) && WEXITSTATUS(st) == 44, 1);
        }
    });

    // An ORPHANED background group gets EIO rather than a stop: nothing
    // outside it could ever continue it, so stopping it would wedge it.
    IN_CHILD({
        setsid();
        open_pty(0, 1, 0);
        if (ioctl(s, TIOCSCTTY, 0) < 0) {
            printf("FAIL could not take a controlling terminal: %s\n", strerror(errno));
            failures_total++;
        } else {
            pid_t job = fork();
            if (job == 0) { setpgid(0, 0); pause(); _exit(0); }
            usleep(200000);
            setpgid(job, job);
            tcsetpgrp(s, job);          // we are background now, and orphaned:
                                        // our parent is in another session
            struct termios t;
            tcgetattr(s, &t);
            DO(tcsetattr(s, TCSANOW, &t));
            ck_err("an orphaned background group gets EIO, not a stop",
                   RC, ER, -1, EIO);
            DO(tcsetpgrp(s, getpgrp()));
            ck_err("  and TIOCSPGRP reports that EIO as ENOTTY", RC, ER, -1, ENOTTY);
            kill(job, SIGKILL);
            int st;
            waitpid(job, &st, 0);
        }
    });

    // ---- a session leader's death frees the terminal -----------------------
    IN_CHILD({
        char name[128];
        int mm, ss;
        if (openpty(&mm, &ss, name, NULL, NULL) < 0) {
            perror("openpty");
            _exit(2);
        }
        fflush(NULL);
        pid_t lead = fork();
        if (lead == 0) {
            setsid();
            if (ioctl(ss, TIOCSCTTY, 0) < 0)
                _exit(3);
            pid_t sv = fork();
            if (sv == 0) { setpgid(0, 0); sleep(6); _exit(0); }
            usleep(300000);
            _exit(0);               // the leader dies; the survivor lives on
        }
        int st;
        waitpid(lead, &st, 0);
        ck("the session leader exited cleanly",
           WIFEXITED(st) && WEXITSTATUS(st) == 0, 1);
        usleep(700000);

        // The terminal must be claimable again even though a member of the
        // old session is still running.
        fflush(NULL);
        pid_t fresh = fork();
        if (fresh == 0) {
            setsid();
            int fd = open(name, O_RDWR | O_NOCTTY);
            if (fd < 0) {
                printf("FAIL could not reopen the pts: %s\n", strerror(errno));
                _exit(1);
            }
            DO(ioctl(fd, TIOCSCTTY, 0));
            _exit(RC == 0 ? 0 : 1);
        }
        waitpid(fresh, &st, 0);
        ck("a new session can claim it while a member survives",
           WIFEXITED(st) && WEXITSTATUS(st) == 0, 1);

        // ...and the device itself was NOT hung up: a pty keeps working for
        // whoever still holds it. Only a real terminal is hung up on exit.
        errno = 0;
        ck("the pts device itself still works", write(ss, "x", 1) == 1, 1);
        struct pollfd p = { ss, POLLIN | POLLHUP, 0 };
        poll(&p, 1, 100);
        ck("  and reports no POLLHUP", (p.revents & POLLHUP) == 0, 1);
        close(mm);
        close(ss);
    });

    return finish_suite("tty_job_control");
}
