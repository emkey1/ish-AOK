// Which wins when the parent directory is unwritable: the permission check, or
// the fact that the final component is not there?
//
// Linux decides it by ORDER, not by policy. Path resolution happens first, so
// do_unlinkat()/do_rmdir() reject a negative dentry before may_delete() is ever
// asked -- a name that does not exist is ENOENT, whatever the parent's mode.
// The create family is the mirror image: filename_create() reports EEXIST for a
// name already there, and only then does may_create() get a say.
//
// AOK checked the parent's write bit during path normalization, ahead of both,
// so every one of these came back EACCES. `rm -f` suppresses ENOENT and nothing
// else, so `rm -f /unwritable/gone` failed where Linux succeeds -- and one such
// rm inside a `set -e` script (the cache store in setup-regressions.sh) killed
// the whole guest suite on the first cache miss, with no diagnostic at all. It
// only ever showed up as a NON-ROOT run, which is why nothing local caught it:
// the CLI harness runs as uid 0, where the parent is always writable and this
// entire ordering question is unreachable.
//
// Every expectation here was measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#define UNPRIV_UID 1000
#define UNPRIV_GID 1000

static char base[160];
static char scratch[320];
static const char *P(const char *sub) {
    snprintf(scratch, sizeof scratch, "%s/%s", base, sub);
    return scratch;
}

// Compare the errno of a call expected to fail. A call that unexpectedly
// SUCCEEDS is its own failure and says so, rather than reporting errno 0.
static void ck_errno(const char *label, int rc, int want) {
    int got = rc == 0 ? 0 : errno;
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-52s got=%-3d want=%d\n", label, got, want);
}

#define AS_USER(...) do {                                                      \
        fflush(NULL);                                                          \
        pid_t c_ = fork();                                                     \
        if (c_ == 0) {                                                         \
            if (setgid(UNPRIV_GID) != 0 || setuid(UNPRIV_UID) != 0) {          \
                printf("FAIL could not drop to uid %d: %s\n",                  \
                       UNPRIV_UID, strerror(errno));                           \
                fflush(NULL);                                                  \
                _exit(1);                                                      \
            }                                                                  \
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

static void rm_rf(const char *path) {
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", path);
    if (system(cmd) < 0)
        return;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    if (geteuid() != 0) {
        // Not a skip we can paper over: with a writable parent the permission
        // check never runs and every case below is vacuous.
        printf("fs_remove_enoent_order: SKIP (needs root to build an unwritable parent)\n");
        return 0;
    }

    snprintf(base, sizeof base, "/tmp/remove_enoent_order.%d", (int) getpid());
    rm_rf(base);
    if (mkdir(base, 0755) != 0) {
        printf("FAIL mkdir %s: %s\n", base, strerror(errno));
        return 1;
    }
    // Owned by root, mode 0755: searchable and readable by everyone, writable
    // by nobody else. That is the shape of /AOK/fakefs/regress-cache, and of
    // "/" itself, which is how an ordinary `rm -f /nope` hit this.
    int fd = open(P("present_file"), O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) close(fd);
    fd = open(P("present_file2"), O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) close(fd);
    if (mkdir(P("present_dir"), 0755) != 0)
        printf("  (mkdir present_dir: %s)\n", strerror(errno));
    if (symlink("/nowhere-at-all", P("dangling")) != 0)
        printf("  (symlink dangling: %s)\n", strerror(errno));
    if (chmod(base, 0755) != 0 || chown(base, 0, 0) != 0)
        printf("  (chmod/chown base: %s)\n", strerror(errno));

    char away[200];
    snprintf(away, sizeof away, "%s.moved", base);

    AS_USER(
        test_logf("-- remove ops, target MISSING: resolution loses to nothing\n");
        ck_errno("unlink(missing)", unlink(P("missing")), ENOENT);
        ck_errno("rmdir(missing)", rmdir(P("missing")), ENOENT);
        ck_errno("rename(missing -> elsewhere)", rename(P("missing"), away), ENOENT);
        ck_errno("unlinkat(AT_FDCWD, missing)",
                 unlinkat(AT_FDCWD, P("missing"), 0), ENOENT);
        ck_errno("unlinkat(missing, AT_REMOVEDIR)",
                 unlinkat(AT_FDCWD, P("missing"), AT_REMOVEDIR), ENOENT);

        test_logf("-- remove ops, target PRESENT: now the parent's mode governs\n");
        ck_errno("unlink(present_file)", unlink(P("present_file")), EACCES);
        ck_errno("rmdir(present_dir)", rmdir(P("present_dir")), EACCES);
        ck_errno("rename(present_file -> elsewhere)",
                 rename(P("present_file"), away), EACCES);
        // A dangling symlink is a name that EXISTS. Resolution must not follow
        // it and call it missing, or unlink would answer the wrong error for
        // the one case where lstat and stat disagree.
        ck_errno("unlink(dangling symlink)", unlink(P("dangling")), EACCES);

        test_logf("-- create ops are the mirror image, and must not regress\n");
        ck_errno("mkdir(missing)", mkdir(P("missing"), 0755), EACCES);
        ck_errno("symlink(-> missing)", symlink("t", P("missing")), EACCES);
        ck_errno("open(missing, O_CREAT)",
                 open(P("missing"), O_CREAT | O_WRONLY, 0644), EACCES);
        ck_errno("mkdir(present_dir) is EEXIST", mkdir(P("present_dir"), 0755), EEXIST);
        ck_errno("symlink(-> present_file) is EEXIST",
                 symlink("t", P("present_file")), EEXIST);

        test_logf("-- a missing PARENT is still ENOENT, not EACCES\n");
        ck_errno("unlink(missing_dir/x)", unlink(P("missing_dir/x")), ENOENT);
        ck_errno("rmdir(missing_dir/x)", rmdir(P("missing_dir/x")), ENOENT);
    );

    rm_rf(base);
    rm_rf(away);
    return finish_suite("fs_remove_enoent_order");
}
