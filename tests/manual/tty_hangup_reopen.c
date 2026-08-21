// A hangup is scoped to the descriptors open when it happened. Linux hands a
// fresh open() of the same terminal a WORKING tty; the EIO belongs to the old
// descriptors, whose terminal really did go away.
//
// AOK kept tty->hung_up set for the life of the tty, so the first session
// leader to exit on a console left it answering EIO forever. The visible
// result was a System Console that worked exactly once per app launch: getty
// opened /dev/tty1, TCGETS returned EIO, getty died, and init eventually gave
// up with `Id "1" respawning too fast: disabled for 5 minutes`. Nothing in
// that message mentions a terminal, which is why it read as a boot problem.
//
// Reproduced here on a pty, because a test may not assume a spare console.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "test_common.h"

static void ck(const char *what, int ok, const char *detail) {
    if (!ok)
        failf(what, (uint64_t) errno, 0, 0, 0, 0, 0);
    test_logf("  %-54s %s%s%s\n", what, ok ? "ok" : "FAIL",
              ok || detail == NULL ? "" : "   ", ok || detail == NULL ? "" : detail);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    // A CONSOLE tty, not a pty. This distinction is the bug: a pty slave is
    // torn down once its users close, so it gets a fresh tty object and a
    // stale hung_up cannot outlive it. A console tty persists for the life of
    // the emulator, which is exactly what let the flag become permanent. A pty
    // version of this test passes even against the broken build.
    //
    // tty2 rather than tty1: tty1 is the System Console, and a test has no
    // business hanging up the terminal somebody may be watching.
    // tty5 rather than tty1: tty1 is the System Console, and a test has no
    // business hanging up a terminal somebody may be watching. Created if the
    // root does not ship it -- a minimal rootfs often has only tty0/tty1.
    static const char slave_path[] = "/dev/tty5";
    int probe = open(slave_path, O_RDWR | O_NOCTTY);
    if (probe < 0 && errno == ENOENT) {
        (void) mknod(slave_path, S_IFCHR | 0600, makedev(4, 5));
        probe = open(slave_path, O_RDWR | O_NOCTTY);
    }
    if (probe < 0) {
        test_logf("  %-54s SKIP (%s)\n", "console tty available", strerror(errno));
        return finish_suite("tty_hangup_reopen");
    }
    close(probe);
    ck("console tty available", 1, NULL);

    // Something must KEEP the tty alive across the hangup, or the object is
    // destroyed when its last user goes and a fresh open gets a brand new one
    // with the flag already clear -- which is why an earlier version of this
    // test passed against the broken build. On the device it is the app's own
    // console view holding /dev/tty1; here it is this descriptor.
    int keeper = open(slave_path, O_RDWR | O_NOCTTY);
    ck("hold the tty open across the hangup", keeper >= 0, strerror(errno));

    // A child that becomes a session leader, takes the slave as its
    // controlling terminal, and exits -- which is what hangs the tty up, and
    // is correct: kernel/exit.c does it deliberately.
    pid_t pid = fork();
    ck("fork", pid >= 0, strerror(errno));
    if (pid == 0) {
        setsid();
        int s = open(slave_path, O_RDWR);
        if (s < 0)
            _exit(11);
        if (ioctl(s, TIOCSCTTY, 0) != 0)
            _exit(12);
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    ck("the session leader took the tty and exited",
       WIFEXITED(status) && WEXITSTATUS(status) == 0, NULL);

    // The hangup has now happened. A FRESH open must give a usable terminal --
    // this is the whole test, and it returned EIO before the fix.
    int reopened = open(slave_path, O_RDWR | O_NOCTTY);
    ck("reopen the slave after the hangup", reopened >= 0, strerror(errno));

    struct termios t;
    errno = 0;
    int rc = tcgetattr(reopened, &t);
    ck("tcgetattr on the reopened tty (was EIO)", rc == 0, strerror(errno));

    // And it must be usable, not merely openable.
    errno = 0;
    ck("tcsetattr on it too", tcsetattr(reopened, TCSANOW, &t) == 0, strerror(errno));

    if (reopened >= 0)
        close(reopened);
    close(keeper);
    return finish_suite("tty_hangup_reopen");
}
