// O_TMPFILE: create an unnamed file on the filesystem holding a directory.
//
//   It arrived as O_DIRECTORY plus a bit AOK did not recognise, so the
//   directory was opened and the write mode then failed it with EISDIR -- an
//   errno that tells the caller it passed a directory, which is exactly what
//   it meant to do. Nothing about that says "this filesystem cannot make an
//   unnamed file", which is the thing the caller needs to know.
//
//   AOK now refuses it with EOPNOTSUPP, the answer Linux gives when the
//   filesystem has no ->tmpfile. That is a deliberate refusal rather than an
//   unfinished implementation. The anonymous file itself would be easy --
//   create a hidden name, open it, unlink it -- but a tmpfile opened WITHOUT
//   O_EXCL can be given a name afterwards with
//   linkat("/proc/self/fd/N", ..., AT_SYMLINK_FOLLOW), and that means linking
//   an inode that has no name, which AOK cannot do. Callers commit to the
//   whole contract the moment open succeeds: systemd's
//   open_tmpfile_linkable() falls back to a named temporary file when the open
//   fails and calls link_tmpfile() when it does not, so succeeding at open and
//   failing at linkat would break precisely the callers that handle the
//   refusal correctly.
//
//   So this test accepts either outcome for the call that can work, and pins
//   the three error cases, which must match Linux exactly either way. If
//   O_TMPFILE is ever implemented here, the linkat check below is what says
//   whether it was implemented completely.
//
// Measured against x86_64 glibc on Linux 6.12 (ext4).
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_common.h"

#ifndef O_TMPFILE
#define O_TMPFILE (020000000 | O_DIRECTORY)
#endif

static char base[80];

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-56s got=%-8ld want=%ld\n", label, got, want);
}

static int count_entries(void) {
    DIR *d = opendir(base);
    if (d == NULL)
        return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
            n++;
    closedir(d);
    return n;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));
    snprintf(base, sizeof base, "/tmp/open-tmpfile-%d", (int) getpid());
    char cmd[160];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd) < 0)
        failures_total++;
    ck("mkdir base", mkdir(base, 0755), 0);

    // ---- the error cases, which must match whatever the answer is ----------
    // An unnamed file you cannot write is of no use to anyone, and Linux
    // decides that from the open flags before any filesystem is consulted --
    // so it is EINVAL even where O_TMPFILE is unsupported.
    errno = 0;
    long r = open(base, O_TMPFILE | O_RDONLY, 0600);
    ck("O_TMPFILE|O_RDONLY is EINVAL", r < 0 ? errno : 0, EINVAL);
    if (r >= 0)
        close((int) r);

    {
        char f[160];
        snprintf(f, sizeof f, "%s/plain", base);
        int t = open(f, O_WRONLY | O_CREAT, 0644);
        ck("create a plain file", t >= 0, 1);
        if (t >= 0)
            close(t);
        errno = 0;
        r = open(f, O_TMPFILE | O_RDWR, 0600);
        ck("O_TMPFILE on a regular file is ENOTDIR", r < 0 ? errno : 0, ENOTDIR);
        if (r >= 0)
            close((int) r);
        unlink(f);
    }
    {
        char missing[200];
        snprintf(missing, sizeof missing, "%s/not-here", base);
        errno = 0;
        r = open(missing, O_TMPFILE | O_RDWR, 0600);
        ck("O_TMPFILE on a missing path is ENOENT", r < 0 ? errno : 0, ENOENT);
        if (r >= 0)
            close((int) r);
    }

    // ---- the call that can work: either it does, completely, or EOPNOTSUPP --
    {
        errno = 0;
        int fd = open(base, O_TMPFILE | O_RDWR, 0600);
        int e = errno;
        if (fd < 0) {
            ck("O_TMPFILE|O_RDWR is EOPNOTSUPP where unsupported", e, EOPNOTSUPP);
            // ...and specifically NOT EISDIR, which is what it used to be and
            // which no caller can act on.
            ck("  and not EISDIR", e == EISDIR ? 1 : 0, 0);
        } else {
            test_logf("  %-56s %s\n", "O_TMPFILE is implemented here", "checking it fully");
            ck("  write to it", (long) write(fd, "hello", 5), 5);
            ck("  seek back", (long) lseek(fd, 0, SEEK_SET), 0);
            char buf[16] = { 0 };
            ck("  read it back", (long) read(fd, buf, 5), 5);
            ck("  with the right contents", strcmp(buf, "hello") == 0, 1);
            struct stat st;
            ck("  fstat", fstat(fd, &st), 0);
            ck("  it is a regular file", S_ISREG(st.st_mode) ? 1 : 0, 1);
            // The whole point: no name anywhere.
            ck("  with no links", (long) st.st_nlink, 0);
            ck("  and invisible in the directory", count_entries(), 0);

            // The half that makes a partial implementation worse than none:
            // without O_EXCL it must be nameable afterwards.
            char proc[64], target[200];
            snprintf(proc, sizeof proc, "/proc/self/fd/%d", fd);
            snprintf(target, sizeof target, "%s/named", base);
            ck("  linkat can give it a name",
               linkat(AT_FDCWD, proc, AT_FDCWD, target, AT_SYMLINK_FOLLOW), 0);
            ck("  and then it is in the directory", count_entries(), 1);
            close(fd);
            unlink(target);

            // ...while WITH O_EXCL it must not be, which is how a caller asks
            // for a file that can never be reached by name.
            int x = open(base, O_TMPFILE | O_RDWR | O_EXCL, 0600);
            ck("  O_EXCL tmpfile opens", x >= 0, 1);
            if (x >= 0) {
                snprintf(proc, sizeof proc, "/proc/self/fd/%d", x);
                snprintf(target, sizeof target, "%s/excl", base);
                errno = 0;
                long lr = linkat(AT_FDCWD, proc, AT_FDCWD, target, AT_SYMLINK_FOLLOW);
                ck("  ...and linkat on it is refused", lr < 0 ? 1 : 0, 1);
                close(x);
            }
        }
    }

    if (system(cmd) < 0)
        failures_total++;
    return finish_suite("open_tmpfile");
}
