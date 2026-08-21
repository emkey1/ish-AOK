// "If the pathname given in pathname is absolute, then dirfd is ignored."
// -- openat(2), and POSIX says the same for the whole *at() family. The
// descriptor is not merely unused: it is not looked at, so it may be -1, or
// closed, or never have been a directory.
//
// AOK validated it regardless and returned EBADF. That is how mariadbd died:
// glibc canonicalising an Aria temp table did
// openat(-1, "/tmp", O_PATH|O_CLOEXEC|O_NOFOLLOW), got EBADF where Linux hands
// back a descriptor, and Aria carried the failure three frames before
// dereferencing the NULL it had left behind. The SIGSEGV landed in
// ha_maria::drop_table, nowhere near the actual mistake, so it read as a
// MariaDB bug for weeks. mysql_install_db never completed either, so no
// MariaDB install on iSH-AOK had ever worked.
//
// The other half matters just as much: a RELATIVE path with a bad dirfd must
// still be EBADF. A fix that returns success for both is worse than the bug.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_common.h"

#ifndef O_PATH
#define O_PATH 010000000
#endif

static void ck(const char *what, int ok, const char *detail) {
    if (!ok)
        failf(what, (uint64_t) errno, 0, 0, 0, 0, 0);
    test_logf("  %-56s %s%s%s\n", what, ok ? "ok" : "FAIL",
              ok || detail == NULL ? "" : "   ", ok || detail == NULL ? "" : detail);
}

// Deliberately not AT_FDCWD, and deliberately not a valid descriptor.
#define JUNK_FD (-1)

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    char dir[] = "/tmp/at_abs_XXXXXX";
    ck("scratch dir", mkdtemp(dir) != NULL, strerror(errno));
    char file[256], link[256], moved[256];
    snprintf(file, sizeof(file), "%s/f", dir);
    snprintf(link, sizeof(link), "%s/l", dir);
    snprintf(moved, sizeof(moved), "%s/m", dir);

    // The exact call mariadbd made.
    int fd = openat(JUNK_FD, "/tmp", O_PATH | O_CLOEXEC | O_NOFOLLOW);
    ck("openat(-1, \"/tmp\", O_PATH|O_CLOEXEC|O_NOFOLLOW)", fd >= 0, strerror(errno));
    if (fd >= 0)
        close(fd);

    fd = openat(JUNK_FD, file, O_CREAT | O_RDWR, 0600);
    ck("openat(-1, abs, O_CREAT|O_RDWR)", fd >= 0, strerror(errno));
    if (fd >= 0) {
        ck("...and it is usable", write(fd, "x", 1) == 1, strerror(errno));
        close(fd);
    }

    struct stat st;
    ck("fstatat(-1, abs)", fstatat(JUNK_FD, file, &st, 0) == 0, strerror(errno));
    ck("faccessat(-1, abs)", faccessat(JUNK_FD, file, R_OK, 0) == 0, strerror(errno));
    ck("symlinkat(target, -1, abs)", symlinkat(file, JUNK_FD, link) == 0, strerror(errno));

    char buf[256];
    ssize_t n = readlinkat(JUNK_FD, link, buf, sizeof(buf) - 1);
    ck("readlinkat(-1, abs)", n > 0, strerror(errno));

    ck("renameat(-1, abs, -1, abs)",
       renameat(JUNK_FD, file, JUNK_FD, moved) == 0, strerror(errno));
    ck("linkat(-1, abs, -1, abs)",
       linkat(JUNK_FD, moved, JUNK_FD, file, 0) == 0, strerror(errno));
    ck("unlinkat(-1, abs)", unlinkat(JUNK_FD, link, 0) == 0, strerror(errno));

    char subdir[256];
    snprintf(subdir, sizeof(subdir), "%s/d", dir);
    ck("mkdirat(-1, abs)", mkdirat(JUNK_FD, subdir, 0700) == 0, strerror(errno));

    // A descriptor that WAS valid and is not any more must behave the same:
    // the rule is about the path, not about the descriptor's history.
    int closed = open("/tmp", O_RDONLY);
    if (closed >= 0)
        close(closed);
    ck("openat(closed_fd, abs)", ({ int f = openat(closed, "/tmp", O_RDONLY);
                                    int ok = f >= 0; if (f >= 0) close(f); ok; }),
       strerror(errno));

    // ...and the half that must NOT change.
    errno = 0;
    ck("openat(-1, RELATIVE) is still EBADF",
       openat(JUNK_FD, "no-such-relative-path", O_RDONLY) == -1 && errno == EBADF,
       strerror(errno));
    errno = 0;
    ck("fstatat(-1, RELATIVE) is still EBADF",
       fstatat(JUNK_FD, "no-such-relative-path", &st, 0) == -1 && errno == EBADF,
       strerror(errno));

    unlinkat(JUNK_FD, moved, 0);
    unlinkat(JUNK_FD, file, 0);
    unlinkat(JUNK_FD, subdir, AT_REMOVEDIR);
    rmdir(dir);
    return finish_suite("at_absolute_path");
}
