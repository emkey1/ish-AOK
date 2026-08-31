// Four unrelated findings that share one shape: answering with something the
// caller cannot act on.
//
//   Writing to a file never dropped its setuid bit. Linux's file_remove_privs
//   exists because a setuid program whose contents just changed is a setuid
//   program somebody else wrote -- so a user with write access to a
//   setuid-root binary could replace it and keep the bit. That is the whole
//   attack the rule stops.
//
//   A syscall number past the end of the dispatch table raised SIGSYS and
//   killed the caller. Linux answers -ENOSYS and nothing else; SIGSYS is
//   seccomp's. Killing instead broke the ordinary way a program finds out
//   whether a syscall exists -- glibc, libseccomp's tests and every
//   call-it-and-see feature probe do exactly this, and here they died
//   mid-probe with no way to catch it.
//
//   getrandom refused any request over 1 MiB with EIO -- a size limit Linux
//   does not have, reported with an errno that says the random source broke
//   rather than that the request was too big. Its flags were ignored
//   completely, so an unknown flag succeeded, which is how a caller probes for
//   a feature.
//
//   personality() refused everything except ADDR_NO_RANDOMIZE, including
//   personality(0) -- the single most common call, and what every "restore the
//   default domain" path does.
//
// Measured against x86_64 glibc on Linux 6.12. The setuid rules are
// privilege-dependent: root holds CAP_FSETID and keeps the bits, so the
// interesting arm only exists as an unprivileged user.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#define UNPRIV_UID 1000
#define UNPRIV_GID 1000
#define GRND_NONBLOCK_ 0x0001
#define GRND_RANDOM_   0x0002
#define GRND_INSECURE_ 0x0004

static char base[80];

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-56s got=%-8ld want=%ld\n", label, got, want);
}

// Create a file at `mode`, write one byte through a fresh fd, and report the
// mode afterwards.
static long mode_after_write(mode_t mode, int by_truncating) {
    char p[128];
    snprintf(p, sizeof p, "%s/f", base);
    unlink(p);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -errno;
    if (write(fd, "seed", 4) != 4) {
        close(fd);
        return -EIO;
    }
    close(fd);
    if (chmod(p, mode) != 0)
        return -errno;
    fd = open(p, O_WRONLY);
    if (fd < 0)
        return -errno;
    if (by_truncating) {
        if (ftruncate(fd, 2) != 0) {
            close(fd);
            return -errno;
        }
    } else if (write(fd, "x", 1) != 1) {
        close(fd);
        return -EIO;
    }
    close(fd);
    struct stat st;
    if (stat(p, &st) != 0)
        return -errno;
    unlink(p);
    return (long) (st.st_mode & 07777);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(90));
    snprintf(base, sizeof base, "/tmp/privs-misc-%d", (int) getpid());
    char cmd[160];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd) < 0)
        failures_total++;
    ck("mkdir base", mkdir(base, 0777), 0);
    chmod(base, 0777);

    // ---- an unknown syscall number is an errno, not a signal ---------------
    // In a child: on a kernel with the bug this is fatal, and a test that dies
    // reports nothing about anything after it.
    {
        fflush(NULL);
        pid_t c = fork();
        if (c == 0) {
            errno = 0;
            long r = syscall(999999);
            _exit(r < 0 && errno == ENOSYS ? 0 : 1);
        }
        int st;
        ck("the child survived syscall(999999)",
           waitpid(c, &st, 0) == c && WIFEXITED(st), 1);
        ck("  and it was ENOSYS", WIFEXITED(st) ? WEXITSTATUS(st) : -1, 0);
    }

    // ---- getrandom ---------------------------------------------------------
    {
        size_t big = 2u * 1024 * 1024;
        char *buf = malloc(big);
        ck("allocate 2 MiB", buf != NULL, 1);
        if (buf != NULL) {
            memset(buf, 0, big);
            errno = 0;
            long r = syscall(SYS_getrandom, buf, big, 0u);
            // Over the old 1 MiB ceiling, and it has to actually fill.
            ck("getrandom of 2 MiB returns it all", r, (long) big);
            // Not just a count: a fill that wrote nothing would pass that.
            int nonzero = 0;
            for (size_t i = big - 4096; i < big; i++)
                if (buf[i] != 0)
                    nonzero = 1;
            ck("  and the far end of the buffer was written", nonzero, 1);
            free(buf);
        }
        char small[16];
        errno = 0;
        ck("getrandom(GRND_NONBLOCK) works",
           (long) syscall(SYS_getrandom, small, (size_t) 16, GRND_NONBLOCK_), 16);
        errno = 0;
        ck("getrandom(GRND_RANDOM) works",
           (long) syscall(SYS_getrandom, small, (size_t) 16, GRND_RANDOM_), 16);
        // An unknown flag must be refused: accepting it tells a caller probing
        // for a feature that the feature is there.
        errno = 0;
        long r = syscall(SYS_getrandom, small, (size_t) 16, 0x40u);
        ck("getrandom with an unknown flag is EINVAL", r < 0 ? errno : 0, EINVAL);
        // ...and the two source flags contradict each other.
        errno = 0;
        r = syscall(SYS_getrandom, small, (size_t) 16, GRND_RANDOM_ | GRND_INSECURE_);
        ck("GRND_RANDOM|GRND_INSECURE is EINVAL", r < 0 ? errno : 0, EINVAL);
        errno = 0;
        ck("getrandom of 0 bytes is 0",
           (long) syscall(SYS_getrandom, small, (size_t) 0, 0u), 0);
    }

    // ---- personality returns the previous value -----------------------------
    // The value itself is not portable: it is whatever the system's default
    // domain is, and AOK's carries ADDR_NO_RANDOMIZE because it does not
    // randomize. What is portable is that a plain call SUCCEEDS and that the
    // query agrees with it.
    {
        errno = 0;
        long first = syscall(SYS_personality, 0xffffffffL);
        ck("personality(0xffffffff) queries without failing", first >= 0, 1);
        errno = 0;
        long set = syscall(SYS_personality, 0L);
        ck("personality(0) succeeds", set >= 0, 1);
        ck("  and returned the previous value", set, first);
        errno = 0;
        long other = syscall(SYS_personality, 0x0800000L /* ADDR_LIMIT_3GB */);
        ck("personality(ADDR_LIMIT_3GB) succeeds", other >= 0, 1);
        errno = 0;
        long back = syscall(SYS_personality, 0xffffffffL);
        ck("  and the query reflects what was set",
           (back & 0x0800000L) != 0 ? 1 : 0, 1);
        // Put it back.
        syscall(SYS_personality, first);
    }

    // ---- writing a file drops its privilege bits ---------------------------
    if (geteuid() == 0) {
        // root holds CAP_FSETID, which skips the whole rule.
        ck("root writing a 4755 file KEEPS the bits", mode_after_write(04755, 0), 04755);
        ck("root ftruncating a 4755 file keeps them", mode_after_write(04755, 1), 04755);

        fflush(NULL);
        pid_t c = fork();
        if (c == 0) {
            failures_total = 0;
            if (setgid(UNPRIV_GID) != 0 || setuid(UNPRIV_UID) != 0) {
                printf("FAIL could not drop privilege: %s\n", strerror(errno));
                fflush(NULL);
                _exit(1);
            }
            ck("unprivileged write to 4755 strips suid", mode_after_write(04755, 0), 0755);
            // Even with no execute bit: Linux kills suid unconditionally.
            ck("  ...and to 4644, which has no exec bit", mode_after_write(04644, 0), 0644);
            ck("  and to 2755 strips sgid", mode_after_write(02755, 0), 0755);
            // The exception: sgid with NO group-execute is a mandatory-locking
            // marker, not a privilege, and Linux leaves it alone. A fix that
            // stripped both bits unconditionally would fail here.
            ck("  but 2644 KEEPS sgid (no group-execute)", mode_after_write(02644, 0), 02644);
            ck("  and 6755 loses both", mode_after_write(06755, 0), 0755);
            ck("  ftruncate counts as a write", mode_after_write(04755, 1), 0755);
            fflush(NULL);
            _exit(failures_total > 250 ? 250 : (int) failures_total);
        }
        int st;
        if (waitpid(c, &st, 0) != c || !WIFEXITED(st)) {
            printf("FAIL unprivileged child did not exit cleanly\n");
            failures_total++;
        } else {
            failures_total += (unsigned) WEXITSTATUS(st);
        }
    } else {
        ck("write to 4755 strips suid", mode_after_write(04755, 0), 0755);
        ck("  but 2644 keeps sgid", mode_after_write(02644, 0), 02644);
    }

    if (system(cmd) < 0)
        failures_total++;
    return finish_suite("privs_syscall_misc");
}
