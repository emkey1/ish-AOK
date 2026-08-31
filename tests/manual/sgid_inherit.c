// A setgid directory hands its group to everything created inside it.
//
//   That is the whole mechanism behind a shared group directory: chgrp the
//   directory to the team's group, set the setgid bit, and every file anyone
//   drops in there belongs to the team rather than to whoever happened to
//   create it. A subdirectory inherits the bit as well, so the arrangement
//   survives one level down and every level after.
//
//   AOK gave every new file the creator's own egid unconditionally, so a
//   setgid directory did nothing at all: the bit was stored, `ls -l` showed
//   the `s`, and the files inside came out with unrelated groups. The failure
//   is silent and it is a permissions failure -- files a group was supposed to
//   share end up readable only by their author, or (with a permissive umask)
//   the reverse.
//
// Measured against x86_64 glibc on Linux 6.12 (ext4).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#define GROUP 1000            // the directory's group
#define OTHER 65534           // the creator's, deliberately different
#define AS_UID 1000

static char base[80];

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-58s got=%-8ld want=%ld\n", label, got, want);
}

static void p(char *out, size_t n, const char *rel) {
    snprintf(out, n, "%s/%s", base, rel);
}

// Everything below is created by an unprivileged process in a different
// group: as root the answer would be the same for the wrong reason, since
// root's own egid could coincidentally match.
static void create_all(const char *dir) {
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        if (setgroups(0, NULL) != 0 || setgid(OTHER) != 0 || setuid(AS_UID) != 0)
            _exit(1);
        char f[200];
        snprintf(f, sizeof f, "%s/file", dir);
        int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0)
            close(fd);
        snprintf(f, sizeof f, "%s/sub", dir);
        mkdir(f, 0755);
        snprintf(f, sizeof f, "%s/link", dir);
        symlink("target", f);
        snprintf(f, sizeof f, "%s/fifo", dir);
        mknod(f, S_IFIFO | 0644, 0);
        // ...and one more level down, through the subdirectory that should
        // have inherited the bit itself.
        snprintf(f, sizeof f, "%s/sub/deep", dir);
        fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0)
            close(fd);
        _exit(0);
    }
    int st;
    waitpid(c, &st, 0);
}

static void group_of(const char *label, const char *path, long want_gid) {
    struct stat s;
    if (lstat(path, &s) != 0) {
        failf(label, (uint64_t) -1, (uint64_t) errno, 0, (uint64_t) want_gid, 0, 0);
        return;
    }
    ck(label, (long) s.st_gid, want_gid);
}

static void mode_of(const char *label, const char *path, long want_mode) {
    struct stat s;
    if (lstat(path, &s) != 0) {
        failf(label, (uint64_t) -1, (uint64_t) errno, 0, (uint64_t) want_mode, 0, 0);
        return;
    }
    ck(label, (long) (s.st_mode & 07777), want_mode);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    if (geteuid() != 0) {
        printf("sgid_inherit: SKIP (needs root to chown a directory to another group)\n");
        return 0;
    }

    snprintf(base, sizeof base, "/tmp/sgid-inherit-%d", (int) getpid());
    char cmd[160];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd) < 0)
        failures_total++;
    ck("mkdir base", mkdir(base, 0755), 0);

    char shared[160], plain[160], f[224];

    // ---- a setgid directory --------------------------------------------
    p(shared, sizeof shared, "shared");
    ck("mkdir the shared directory", mkdir(shared, 0777), 0);
    ck("  chgrp it to the group", chown(shared, 0, GROUP), 0);
    ck("  set the setgid bit", chmod(shared, 02777), 0);
    create_all(shared);

    p(f, sizeof f, "shared/file");
    group_of("a file takes the DIRECTORY's group", f, GROUP);
    // ...but not the bit: S_ISGID on a plain file means something else
    // entirely (setgid-on-exec), and Linux does not set it here.
    mode_of("  and is not itself setgid", f, 0644);

    p(f, sizeof f, "shared/sub");
    group_of("a subdirectory takes it too", f, GROUP);
    mode_of("  and inherits the setgid bit", f, 02755);

    p(f, sizeof f, "shared/link");
    group_of("a symlink takes it", f, GROUP);

    p(f, sizeof f, "shared/fifo");
    group_of("a fifo takes it", f, GROUP);

    // The point of the bit propagating: it keeps working further down.
    p(f, sizeof f, "shared/sub/deep");
    group_of("a file one level deeper takes it as well", f, GROUP);

    // ---- the control: an ordinary directory -----------------------------
    // Without this a kernel that simply always used the parent's group would
    // pass everything above.
    p(plain, sizeof plain, "plain");
    ck("mkdir an ordinary directory", mkdir(plain, 0777), 0);
    ck("  chgrp it to the same group", chown(plain, 0, GROUP), 0);
    ck("  with NO setgid bit", chmod(plain, 0777), 0);
    create_all(plain);

    p(f, sizeof f, "plain/file");
    group_of("a file there takes the CREATOR's group", f, OTHER);
    p(f, sizeof f, "plain/sub");
    group_of("so does a subdirectory", f, OTHER);
    mode_of("  which is not setgid", f, 0755);

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd) < 0)
        failures_total++;
    return finish_suite("sgid_inherit");
}
