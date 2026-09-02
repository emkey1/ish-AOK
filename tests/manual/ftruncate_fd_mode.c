// ftruncate(2) takes its permission from the DESCRIPTOR, not from the inode's
// mode bits. Linux's do_sys_ftruncate checks only FMODE_WRITE, and returns
// EINVAL -- not EACCES -- when the file was not opened for writing. Only the
// path-based truncate(2) consults the inode, because only it has to resolve a
// path in the first place.
//
// AOK briefly ran the inode check on both. generic_fsetattr, which backs
// ftruncate/fchmod/fchown, called setattr_check(), whose attr_size case is
// access_check(AC_W). A writable descriptor to a file whose mode bits deny
// writing therefore failed, even though write(2) on the very same descriptor
// succeeded -- self-inconsistent, and wrong against Linux either way.
//
// That combination is not exotic; it is how wlroots allocates every keyboard
// keymap (util/shm.c allocate_shm_file_pair):
//
//     open("/dev/shm/wlroots-XXXXXX", O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW, 0600)
//     open("/dev/shm/wlroots-XXXXXX", O_RDONLY|O_NOFOLLOW)   // read-only peer
//     fchmod(rw_fd, 000)          // so nobody can reopen it
//     ftruncate(rw_fd, keymap_size)
//
// Under the bug that ftruncate returned EACCES, so wlr_keyboard_set_keymap
// failed ("Failed to allocate shm file for keymap"), the keyboard kept a NULL
// keymap, and labwc then called xkb_state_get_keymap(NULL) -- an 8-byte
// accessor that is just `LDR X0,[X0,#0x80]; RET` -- and took SIGSEGV at 0x80
// before it ever created its Wayland socket. The entire Wayland desktop failed
// to start. (It faulted rather than limping only because the NULL page is no
// longer auto-mapped; before that fix the bad read silently returned zero.)
//
// Runs unprivileged on purpose: setattr_check short-circuits on superuser(),
// so as root this passes with or without the fix.
//
// Verified against real Linux (mint oracle): every case below behaves
// identically there.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#define UNPRIV_UID 1000
#define UNPRIV_GID 1000

static void check_ok(const char *what, int r) {
    if (r >= 0) {
        test_logf("  ok   %s -> %d\n", what, r);
        return;
    }
    printf("FAIL: %s -> %s (want success)\n", what, strerror(errno));
    failures_total++;
}

static void check_errno(const char *what, int r, int want) {
    if (r < 0 && errno == want) {
        test_logf("  ok   %s -> %s\n", what, strerror(errno));
        return;
    }
    if (r >= 0)
        printf("FAIL: %s unexpectedly succeeded (want %s)\n", what, strerror(want));
    else
        printf("FAIL: %s -> %s (want %s)\n", what, strerror(errno), strerror(want));
    failures_total++;
}

// The wlroots keymap sequence, on whichever writable filesystem we're given.
static void wlroots_keymap_pattern(const char *dir) {
    char path[256];
    snprintf(path, sizeof path, "%s/ftrunc-fd-mode-%d", dir, (int) getpid());
    unlink(path);

    int rw = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (rw < 0) {
        test_logf("  skip %s: %s\n", dir, strerror(errno));
        return;
    }
    int ro = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    check_ok("reopen read-only", ro);

    // Drop every permission bit, exactly as wlroots does, so the file cannot be
    // reopened by anyone. The descriptors already held stay fully usable.
    errno = 0;
    check_ok("fchmod(rw, 000)", fchmod(rw, 0));

    struct stat st;
    if (fstat(rw, &st) == 0 && (st.st_mode & 07777) != 0) {
        printf("FAIL: mode after fchmod(000) is %04o\n", (unsigned) (st.st_mode & 07777));
        failures_total++;
    }

    // The point of the test: the descriptor is writable, so this must work.
    errno = 0;
    check_ok("ftruncate through the writable fd on a mode-000 file",
             ftruncate(rw, 35572));

    // write(2) always honoured the descriptor; assert it still does, so the two
    // can't drift apart again.
    errno = 0;
    check_ok("write through that same fd", (int) write(rw, "x", 1));

    // ... and the descriptor really is the authority, in both directions: a
    // read-only one is refused, with EINVAL rather than EACCES.
    errno = 0;
    check_errno("ftruncate through a read-only fd", (int) ftruncate(ro, 10), EINVAL);

    // A plain owner-writable file is the ordinary case; keep it covered.
    close(rw);
    close(ro);
    unlink(path);

    rw = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (rw >= 0) {
        errno = 0;
        check_ok("ftruncate on an ordinary 0600 file", ftruncate(rw, 1024));
        close(rw);
    }
    unlink(path);
}

static int run_unprivileged(void) {
    test_logf("  running as uid %d\n", (int) getuid());
    wlroots_keymap_pattern("/tmp");
    wlroots_keymap_pattern("/dev/shm");
    return failures_total != 0;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    if (geteuid() != 0) {
        run_unprivileged();
        return finish_suite("ftruncate_fd_mode");
    }

    // As root the check under test is skipped entirely (superuser() returns
    // early), so drop privileges in a child and judge by its exit status.
    pid_t pid = fork();
    if (pid < 0) {
        printf("ftruncate_fd_mode: FAIL fork: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        if (setgid(UNPRIV_GID) != 0 || setuid(UNPRIV_UID) != 0) {
            printf("ftruncate_fd_mode: SKIP (cannot drop privileges: %s)\n", strerror(errno));
            fflush(NULL);
            _exit(42);
        }
        int bad = run_unprivileged();
        printf("  (child) %u failure(s)\n", failures_total);
        fflush(NULL);
        _exit(bad ? 1 : 0);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status)) {
        printf("ftruncate_fd_mode: FAIL child did not exit normally\n");
        return 1;
    }
    if (WEXITSTATUS(status) == 42) {
        printf("ftruncate_fd_mode: SKIP (could not drop privileges)\n");
        return 0;
    }
    if (WEXITSTATUS(status) != 0)
        failures_total++;
    return finish_suite("ftruncate_fd_mode");
}
