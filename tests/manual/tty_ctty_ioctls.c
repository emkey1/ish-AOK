// Controlling-terminal ownership, and the terminal ioctls AOK never wired up.
//
// Two groups of defects, both of which a normal program hits:
//
//   TIOCSCTTY did not check that the caller was a session leader. Linux
//   requires it (signal->leader), and without the check ANY process could take
//   a terminal that another session was using -- which hangs that session up.
//   The probe for this kept killing its own shell, which is what the bug does.
//
//   tcdrain(), tcflow(), tcsendbreak() and TIOCOUTQ reached the kernel as
//   TCSBRK / TCXONC / TIOCOUTQ and fell through to ENOTTY. ENOTTY is how a
//   program decides it is not talking to a terminal at all, so a caller that
//   drains before reading its own output saw a failure a real tty never gives.
//
//   TIOCEXCL was accepted and enforced nothing -- worse than absent, because a
//   program keeping a second reader off its line was told it had succeeded.
//   TIOCNOTTY was ENOTTY, so a process daemonising by hand could not detach.
//
// Every expectation here was measured against x86_64 glibc on Linux 6.12,
// including the two that depend on privilege (stealing a terminal, and opening
// an exclusive one) -- root is exempt from both, on Linux exactly as here, so
// those run both arms when the suite runs as root.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>

#include "test_common.h"

// An unprivileged uid to drop to for the privilege-sensitive arms. Any nonzero
// uid does; nothing is created or written as it.
#define UNPRIV_UID 65534

static int RC, ER;
// errno must be read after the call completes and never as a sibling argument
// -- argument evaluation order is unspecified, and gcc really does read errno
// first, which quietly turned a correct result into a reported failure here.
#define DO(call) do { errno = 0; RC = (call); ER = errno; } while (0)

static void ck(const char *label, int rc, int er, int want_rc, int want_errno) {
    int ok = rc == want_rc && (rc >= 0 || er == want_errno);
    if (!ok)
        failf(label, (uint64_t) rc, (uint64_t) er, 0,
              (uint64_t) want_rc, (uint64_t) want_errno, 0);
    test_logf("  %-52s rc=%-3d errno=%-2d want rc=%-3d errno=%d\n",
              label, rc, rc < 0 ? er : 0, want_rc, want_errno);
}

// Run body in a child so a session change or a stolen terminal cannot poison
// the rest of the suite, and fold its failures back into ours.
#define IN_CHILD(...) do {                                                    \
        fflush(NULL);                                                          \
        pid_t c_ = fork();                                                     \
        if (c_ == 0) {                                                         \
            /* report only OUR failures: inheriting the parent's count and     \
               adding it back makes a single failure double per block. */      \
            failures_total = 0;                                                \
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

static int slave_fd(char *name, size_t cap) {
    int m, s;
    char buf[128];
    if (openpty(&m, &s, buf, NULL, NULL) < 0) {
        perror("openpty");
        _exit(2);
    }
    if (name != NULL)
        snprintf(name, cap, "%s", buf);
    return s;
}

static volatile sig_atomic_t got_hup, got_cont;
static void note_hup(int n)  { (void) n; got_hup = 1; }
static void note_cont(int n) { (void) n; got_cont = 1; }

static void *claim_from_thread(void *arg) {
    DO(ioctl(*(int *) arg, TIOCSCTTY, 0));
    ck("a thread of the session leader may claim it", RC, ER, 0, 0);
    return NULL;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    // ---- the ioctls that were ENOTTY -------------------------------------
    {
        int s = slave_fd(NULL, 0);
        DO(tcdrain(s));
        ck("tcdrain", RC, ER, 0, 0);
        DO(tcsendbreak(s, 0));
        ck("tcsendbreak", RC, ER, 0, 0);
        for (int i = 0; i < 4; i++) {
            static const int act[] = { TCOOFF, TCOON, TCIOFF, TCION };
            static const char *nm[] = { "TCOOFF", "TCOON", "TCIOFF", "TCION" };
            DO(tcflow(s, act[i]));
            char label[64];
            snprintf(label, sizeof label, "tcflow(%s)", nm[i]);
            ck(label, RC, ER, 0, 0);
        }
        DO(ioctl(s, TCXONC, 99));
        ck("tcflow with a bad action is EINVAL", RC, ER, -1, EINVAL);
        int q = -1;
        DO(ioctl(s, TIOCOUTQ, &q));
        ck("TIOCOUTQ", RC, ER, 0, 0);
        ck("  reports nothing queued", q, 0, 0, 0);
        close(s);
    }

    // ---- TIOCEXCL is enforced, not merely recorded ------------------------
    IN_CHILD({
        char name[128];
        int s = slave_fd(name, sizeof name);
        // The pts node is created mode 0620 owned by us, so a caller with a
        // different uid is refused by the ordinary permission check before it
        // ever reaches the exclusivity one. Open it up first, or the
        // unprivileged arm below measures the wrong thing (EACCES, not EBUSY).
        if (fchmod(s, 0666) != 0) {
            printf("FAIL fchmod on a pts: %s\n", strerror(errno));
            failures_total++;
        }
        DO(ioctl(s, TIOCEXCL, 0));
        ck("TIOCEXCL", RC, ER, 0, 0);
        int excl = -1;
        DO(ioctl(s, TIOCGEXCL, &excl));
        ck("TIOCGEXCL", RC, ER, 0, 0);
        ck("  reads the flag back", excl, 0, 1, 0);
        // Privileged callers are exempt (Linux: CAP_SYS_ADMIN), so as root the
        // open succeeds and the refusal has to be checked from a child that
        // has dropped privilege.
        if (geteuid() == 0) {
            DO(open(name, O_RDWR | O_NOCTTY));
            ck("root opens an exclusive terminal", RC >= 0 ? 0 : -1, ER, 0, 0);
            if (RC >= 0)
                close(RC);
            IN_CHILD({
                if (setgid(UNPRIV_UID) != 0 || setuid(UNPRIV_UID) != 0) {
                    printf("FAIL could not drop to uid %d: %s\n",
                           UNPRIV_UID, strerror(errno));
                    failures_total++;
                } else {
                    DO(open(name, O_RDWR | O_NOCTTY));
                    ck("unprivileged open of an exclusive terminal",
                       RC >= 0 ? 0 : -1, ER, -1, EBUSY);
                    if (RC >= 0)
                        close(RC);
                }
            });
        } else {
            DO(open(name, O_RDWR | O_NOCTTY));
            ck("unprivileged open of an exclusive terminal",
               RC >= 0 ? 0 : -1, ER, -1, EBUSY);
            if (RC >= 0)
                close(RC);
        }
        DO(ioctl(s, TIOCNXCL, 0));
        ck("TIOCNXCL", RC, ER, 0, 0);
        DO(open(name, O_RDWR | O_NOCTTY));
        ck("open after TIOCNXCL", RC >= 0 ? 0 : -1, ER, 0, 0);
        if (RC >= 0)
            close(RC);
    });

    // ---- who may claim a controlling terminal -----------------------------
    IN_CHILD({
        int s = slave_fd(NULL, 0);
        DO(ioctl(s, TIOCSCTTY, 0));
        ck("a non-session-leader is refused", RC, ER, -1, EPERM);
    });

    IN_CHILD({
        setsid();
        int s = slave_fd(NULL, 0);
        DO(ioctl(s, TIOCSCTTY, 0));
        ck("a session leader with no terminal may claim one", RC, ER, 0, 0);
        DO(ioctl(s, TIOCSCTTY, 0));
        ck("re-claiming the one we already hold is a no-op", RC, ER, 0, 0);
        int s2 = slave_fd(NULL, 0);
        DO(ioctl(s2, TIOCSCTTY, 0));
        ck("a second terminal while we hold one is refused", RC, ER, -1, EPERM);
    });

    IN_CHILD({
        // signal->leader is a property of the PROCESS, so a thread of the
        // session leader is inside a session leader. Comparing the sid against
        // the calling task's pid gets this wrong for every thread but one.
        setsid();
        int s = slave_fd(NULL, 0);
        pthread_t t;
        if (pthread_create(&t, NULL, claim_from_thread, &s) != 0) {
            printf("FAIL pthread_create: %s\n", strerror(errno));
            failures_total++;
        } else {
            pthread_join(t, NULL);
        }
    });

    // ---- stealing one that belongs to another session ---------------------
    IN_CHILD({
        int s = slave_fd(NULL, 0);
        fflush(NULL);
        pid_t owner = fork();
        if (owner == 0) {
            setsid();
            ioctl(s, TIOCSCTTY, 0);
            pause();
            _exit(0);
        }
        usleep(300000);
        if (geteuid() == 0) {
            IN_CHILD({
                if (setgid(UNPRIV_UID) != 0 || setuid(UNPRIV_UID) != 0) {
                    printf("FAIL could not drop to uid %d: %s\n",
                           UNPRIV_UID, strerror(errno));
                    failures_total++;
                } else {
                    setsid();
                    DO(ioctl(s, TIOCSCTTY, 1));
                    ck("stealing another session's terminal, unprivileged",
                       RC, ER, -1, EPERM);
                }
            });
        }
        setsid();
        DO(ioctl(s, TIOCSCTTY, 1));
        if (geteuid() == 0)
            ck("stealing another session's terminal, as root", RC, ER, 0, 0);
        else
            ck("stealing another session's terminal, unprivileged",
               RC, ER, -1, EPERM);
        kill(owner, SIGKILL);
        int st;
        waitpid(owner, &st, 0);
    });

    // ---- TIOCNOTTY, both of the operations it names -----------------------
    IN_CHILD({
        int s = slave_fd(NULL, 0);
        DO(ioctl(s, TIOCNOTTY, 0));
        ck("TIOCNOTTY on a terminal that is not ours", RC, ER, -1, ENOTTY);
    });

    IN_CHILD({
        // A session LEADER hangs the session up: SIGHUP then SIGCONT to the
        // terminal's foreground group, which includes us -- so we have to
        // survive our own hangup to report anything.
        signal(SIGHUP, note_hup);
        signal(SIGCONT, note_cont);
        setsid();
        int s = slave_fd(NULL, 0);
        if (ioctl(s, TIOCSCTTY, 0) < 0) {
            printf("FAIL could not take a controlling terminal: %s\n",
                   strerror(errno));
            failures_total++;
        } else {
            DO(open("/dev/tty", O_RDWR));
            ck("/dev/tty resolves while we hold a terminal",
               RC >= 0 ? 0 : -1, ER, 0, 0);
            if (RC >= 0)
                close(RC);
            DO(ioctl(s, TIOCNOTTY, 0));
            ck("TIOCNOTTY on our own controlling terminal", RC, ER, 0, 0);
            usleep(300000);
            ck("  hangs up the foreground group (SIGHUP)", got_hup, 0, 1, 0);
            ck("  and follows it with SIGCONT", got_cont, 0, 1, 0);
            DO(open("/dev/tty", O_RDWR));
            ck("  /dev/tty stops resolving afterwards",
               RC >= 0 ? 0 : -1, ER, -1, ENXIO);
            if (RC >= 0)
                close(RC);
        }
    });

    IN_CHILD({
        // ...while anyone else drops only their own reference and leaves the
        // session, its terminal and its other members alone.
        setsid();
        int s = slave_fd(NULL, 0);
        if (ioctl(s, TIOCSCTTY, 0) < 0) {
            printf("FAIL could not take a controlling terminal: %s\n",
                   strerror(errno));
            failures_total++;
        } else {
            IN_CHILD({
                DO(ioctl(s, TIOCNOTTY, 0));
                ck("TIOCNOTTY as a non-leader", RC, ER, 0, 0);
                DO(open("/dev/tty", O_RDWR));
                ck("  drops our own reference", RC >= 0 ? 0 : -1, ER, -1, ENXIO);
                if (RC >= 0)
                    close(RC);
            });
            DO(open("/dev/tty", O_RDWR));
            ck("  and leaves the session leader's terminal alone",
               RC >= 0 ? 0 : -1, ER, 0, 0);
            if (RC >= 0)
                close(RC);
        }
    });

    return finish_suite("tty_ctty_ioctls");
}
