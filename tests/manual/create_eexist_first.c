// Creating a name that already exists, in a directory you cannot write.
//
// Linux looks the final component up BEFORE checking whether the parent may
// be written: filename_create() returns -EEXIST for a name already there, and
// only vfs_mkdir/vfs_link/etc. then ask may_create() for permission. AOK
// checked the parent first and reported EACCES.
//
// That broke `mkdir -p` for every unprivileged user: it calls mkdir on each
// component and treats EEXIST as success, so `mkdir -p /tmp/anything` failed
// outright -- "/" is not writable by an ordinary user and /tmp already exists.
// Found because ktop's build script does exactly that, and a non-root user
// could not re-run it.
//
// The deferral must not GRANT anything, so the second half checks that
// creating a name that does NOT exist under an unwritable parent is still
// refused, and that unlink/rmdir/rename -- where an existing target is the
// point of the call and permission is what governs -- are unaffected.
//
// Every expectation was measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "test_common.h"

static void check(const char *label, int r, int want_errno) {
    int e = r < 0 ? errno : 0;
    if (!(r < 0 && e == want_errno))
        failf(label, (uint64_t) r, (uint64_t) e, 0, (uint64_t) -1, (uint64_t) want_errno, 0);
    test_logf("  %-42s rc=%d errno=%d (want %d)\n", label, r, e, want_errno);
}

// Runs as an unprivileged uid in a child: root bypasses the parent's write
// permission entirely, so the whole point is invisible to it.
static int child_main(const char *dir, const char *existing) {
    char newname[512];
    snprintf(newname, sizeof newname, "%s/does-not-exist", dir);

    // Already exists -> EEXIST wins over the unwritable parent.
    errno = 0; check("mkdir(existing)",   mkdir(existing, 0755), EEXIST);
    errno = 0; check("symlink(existing)", symlink("x", existing), EEXIST);
    errno = 0; check("mknod(existing)",   mknod(existing, S_IFIFO | 0644, 0), EEXIST);
    errno = 0; {
        int fd = open(existing, O_CREAT | O_EXCL | O_WRONLY, 0644);
        check("open(existing, O_CREAT|O_EXCL)", fd < 0 ? -1 : 0, EEXIST);
        if (fd >= 0) close(fd);
    }

    // Does NOT exist -> the parent's permission still refuses it.
    errno = 0; check("mkdir(new)",   mkdir(newname, 0755), EACCES);
    errno = 0; check("symlink(new)", symlink("x", newname), EACCES);
    errno = 0; check("mknod(new)",   mknod(newname, S_IFIFO | 0644, 0), EACCES);
    errno = 0; {
        int fd = open(newname, O_CREAT | O_WRONLY, 0644);
        check("open(new, O_CREAT)", fd < 0 ? -1 : 0, EACCES);
        if (fd >= 0) close(fd);
    }

    // Removal and rename are governed by permission, not existence.
    errno = 0; check("unlink(existing)", unlink(existing), EACCES);
    // Removing a directory owned by someone else: EACCES normally, but EPERM
    // when the parent is sticky, which /tmp is (01777). Accept either -- the
    // point is that it is refused, not which of the two rules refused it.
    errno = 0;
    {
        int r = rmdir(dir);
        int e = r < 0 ? errno : 0;
        if (!(r < 0 && (e == EACCES || e == EPERM)))
            failf("rmdir(dir-in-unwritable-parent)", (uint64_t) r, (uint64_t) e, 0,
                  (uint64_t) -1, (uint64_t) EACCES, 0);
        test_logf("  %-42s rc=%d errno=%d (want 13 or 1)\n",
                  "rmdir(dir-in-unwritable-parent)", r, e);
    }

    // And mkdir -p's actual pattern, which is what surfaced this.
    char deep[512];
    snprintf(deep, sizeof deep, "%s/sub", dir);
    errno = 0;
    if (mkdir(deep, 0755) == 0)
        failf("mkdir under an unwritable dir must fail", 0, 0, 0, 0, 0, 0);

    return failures_total == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    if (geteuid() != 0) {
        printf("create_eexist_first: SKIP (needs root to drop to an unprivileged uid)\n");
        return 0;
    }

    // A directory the child cannot write, holding a name that already exists.
    char dir[256], existing[512];
    snprintf(dir, sizeof dir, "/tmp/eexist_first.%d", (int) getpid());
    if (mkdir(dir, 0755) < 0) {
        printf("create_eexist_first: SKIP (mkdir %s: %s)\n", dir, strerror(errno));
        return 0;
    }
    snprintf(existing, sizeof existing, "%s/already-here", dir);
    int fd = open(existing, O_WRONLY | O_CREAT, 0644);
    if (fd >= 0) close(fd);

    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        // 1500 rather than 1000: some roots already have a user there.
        if (setgid(1500) < 0 || setuid(1500) < 0)
            _exit(70);
        int rc = child_main(dir, existing);
        fflush(NULL);        // _exit skips the flush; the diagnosis lives here
        _exit(rc);
    }
    int st = 0;
    while (waitpid(c, &st, 0) < 0 && errno == EINTR)
        continue;
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    if (code == 70) {
        printf("create_eexist_first: SKIP (cannot drop privileges)\n");
    } else if (code != 0) {
        failf("unprivileged create-on-existing", (uint64_t) code, (uint64_t) st, 0, 0, 0, 0);
    }

    unlink(existing);
    rmdir(dir);
    return finish_suite("create_eexist_first");
}
