// Five permission and path-resolution rules AOK got wrong, two of them
// security-relevant and two of them plainly visible at a shell prompt.
//
//   utimensat performed no check at all -- generic_utime went straight to the
//   filesystem, unlike generic_setattrat beside it -- so any user could
//   restamp any file on the system, including root-owned ones they could not
//   write. That alone is enough to mislead make, rsync, tar and every backup
//   tool that trusts an mtime.
//
//   access(2) deliberately swaps the fs ids to the REAL uid so a setuid-root
//   program can ask "could the user who ran me read this?" -- but the root
//   override in access_check consulted the EFFECTIVE uid, which the swap does
//   not touch. So the answer was always yes, which is the opposite of what the
//   caller wanted to know, and exactly the check a setuid program makes before
//   opening a file on the user's behalf.
//
//   chdir()/chroot() opened the directory O_RDONLY, so they demanded read
//   permission where Linux wants only search. `cd` into a 0711 directory --
//   the standard shape for a home directory or a drop-box -- simply failed.
//
//   A trailing slash made path resolution demand execute permission on the
//   FINAL component, because the resolver could not tell "traverse into this"
//   from "this must be a directory". stat("/root/") was EACCES for an ordinary
//   user where stat("/root") succeeded, so `test -d /root/` and `ls -d /root/`
//   failed too.
//
//   Symlink resolution was capped at 5 followed links against Linux's 40, and
//   the count includes symlinked directory components, so ordinary
//   /etc/alternatives-style chains hit a spurious ELOOP.
//
// Measured against x86_64 glibc on Linux 6.12. Needs root, to create the
// fixtures and to drop to an unprivileged uid.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#include "test_common.h"

// Any nonzero uid does; nothing is created or written as it, so it does not
// need to exist in /etc/passwd.
#define UNPRIV_UID 1000
#define UNPRIV_GID 1000

static char base[160];
static char scratch[320];
static const char *P(const char *sub) {
    snprintf(scratch, sizeof scratch, "%s/%s", base, sub);
    return scratch;
}

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-56s got=%-6ld want=%ld\n", label, got, want);
}

// Run a block as an unprivileged user, folding its failures back into ours.
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
    alarm(test_watchdog_secs(60));

    if (geteuid() != 0) {
        printf("fs_permission_rules: SKIP (needs root to build the fixtures "
               "and to drop privilege)\n");
        return 0;
    }
    snprintf(base, sizeof base, "/tmp/fsperm-%d", (int) getpid());
    rm_rf(base);
    if (mkdir(base, 0755) != 0) {
        printf("FAIL mkdir %s: %s\n", base, strerror(errno));
        return finish_suite("fs_permission_rules");
    }

    // Fixtures, all owned by root.
    char d711[320], d111[320], d700[320], secret[320], stamped[320];
    snprintf(d711, sizeof d711, "%s", P("d711"));
    snprintf(d111, sizeof d111, "%s", P("d111"));
    snprintf(d700, sizeof d700, "%s", P("d700"));
    snprintf(secret, sizeof secret, "%s", P("secret"));
    snprintf(stamped, sizeof stamped, "%s", P("stamped"));
    ck("fixtures: 0711 dir", mkdir(d711, 0711), 0);
    ck("fixtures: 0111 dir", mkdir(d111, 0111), 0);
    ck("fixtures: 0700 dir", mkdir(d700, 0700), 0);
    { int f = open(P("d711/f"), O_RDWR | O_CREAT, 0644); ck("fixtures: file inside it", f >= 0, 1); if (f >= 0) close(f); }
    { int f = open(secret, O_RDWR | O_CREAT, 0600); ck("fixtures: root-only file", f >= 0, 1); if (f >= 0) close(f); }
    { int f = open(stamped, O_RDWR | O_CREAT, 0644); ck("fixtures: 0644 file", f >= 0, 1); if (f >= 0) close(f); }

    // An 8-deep symlink chain: lc_8 -> lc_7 -> ... -> lc_1 -> lc_real.
    { int f = open(P("lc_real"), O_RDWR | O_CREAT, 0644); if (f >= 0) { if (write(f, "END\n", 4) != 4) {} close(f); } }
    for (int i = 1; i <= 8; i++) {
        char from[320], to[320];
        snprintf(to, sizeof to, "%s/lc_%d", base, i);
        if (i == 1)
            snprintf(from, sizeof from, "%s/lc_real", base);
        else
            snprintf(from, sizeof from, "%s/lc_%d", base, i - 1);
        if (symlink(from, to) != 0) {
            printf("FAIL symlink %s: %s\n", to, strerror(errno));
            failures_total++;
        }
    }

    // ---- symlink chains up to Linux's MAXSYMLINKS -------------------------
    for (int n = 5; n <= 8; n++) {
        char p[320], label[80];
        snprintf(p, sizeof p, "%s/lc_%d", base, n);
        errno = 0;
        int fd = open(p, O_RDONLY);
        snprintf(label, sizeof label, "a chain of %d symlinks resolves", n);
        ck(label, fd >= 0 ? 0 : errno, 0);
        if (fd >= 0)
            close(fd);
    }

    // ---- chdir wants search permission, not read --------------------------
    AS_USER({
        errno = 0;
        ck("chdir into a 0711 root-owned directory", chdir(d711) < 0 ? errno : 0, 0);
        errno = 0;
        ck("chdir into a 0111 root-owned directory", chdir(d111) < 0 ? errno : 0, 0);
        errno = 0;
        ck("  but 0700 is still refused", chdir(d700) < 0 ? errno : 0, EACCES);
        char inner[400];
        snprintf(inner, sizeof inner, "%s/f", d711);
        errno = 0;
        int f = open(inner, O_RDONLY);
        ck("  and a file inside the 0711 directory still opens", f < 0 ? errno : 0, 0);
        if (f >= 0)
            close(f);
    });

    // ---- a trailing slash asks "is it a directory", not "let me in" -------
    AS_USER({
        struct stat st;
        char with_slash[400], with_two[400], with_dot[400];
        snprintf(with_slash, sizeof with_slash, "%s/", d700);
        snprintf(with_two, sizeof with_two, "%s//", d700);
        snprintf(with_dot, sizeof with_dot, "%s/.", d700);
        errno = 0; ck("stat(dir) on an unreadable directory", stat(d700, &st) < 0 ? errno : 0, 0);
        errno = 0; ck("stat(dir/) likewise", stat(with_slash, &st) < 0 ? errno : 0, 0);
        errno = 0; ck("stat(dir//) likewise", stat(with_two, &st) < 0 ? errno : 0, 0);
        errno = 0; ck("  but dir/. IS inside it, so EACCES", stat(with_dot, &st) < 0 ? errno : 0, EACCES);
        errno = 0; ck("access(dir/, F_OK)", access(with_slash, F_OK) < 0 ? errno : 0, 0);
        errno = 0; ck("lstat(dir/)", lstat(with_slash, &st) < 0 ? errno : 0, 0);
    });

    // ---- utimensat is a permission-checked operation ----------------------
    AS_USER({
        struct timespec times[2] = { { 946684800, 0 }, { 946684800, 0 } };
        errno = 0;
        ck("explicit times on a file we do not own is EPERM",
           utimensat(AT_FDCWD, stamped, times, 0) < 0 ? errno : 0, EPERM);
        errno = 0;
        ck("  \"now\" on a file we cannot write is EACCES",
           utimensat(AT_FDCWD, stamped, NULL, 0) < 0 ? errno : 0, EACCES);
    });

    // ...and still works for the owner, which is the whole point of having it.
    {
        char owned[400];
        snprintf(owned, sizeof owned, "%s/owned", base);
        int f = open(owned, O_RDWR | O_CREAT, 0644);
        if (f >= 0)
            close(f);
        ck("a file handed to the user", chown(owned, UNPRIV_UID, UNPRIV_GID), 0);
        AS_USER({
            struct timespec times[2] = { { 946684800, 0 }, { 946684800, 0 } };
            errno = 0;
            ck("the owner may set explicit times",
               utimensat(AT_FDCWD, owned, times, 0) < 0 ? errno : 0, 0);
            struct stat st;
            ck("  and they took", stat(owned, &st) == 0 && st.st_mtime == 946684800, 1);
            errno = 0;
            ck("the owner may touch it to \"now\"",
               utimensat(AT_FDCWD, owned, NULL, 0) < 0 ? errno : 0, 0);
        });
    }

    // ---- access(2) answers about the REAL uid -----------------------------
    {
        // Exactly the state of a setuid-root program: uid unprivileged, euid 0.
        fflush(NULL);
        pid_t c = fork();
        if (c == 0) {
            if (setresgid(UNPRIV_GID, 0, 0) != 0 || setresuid(UNPRIV_UID, 0, 0) != 0) {
                printf("FAIL setresuid: %s\n", strerror(errno));
                fflush(NULL);
                _exit(1);
            }
            failures_total = 0;
            ck("setuid-root state: uid is unprivileged", getuid() == UNPRIV_UID, 1);
            ck("  and euid is root", geteuid() == 0, 1);
            errno = 0;
            ck("access(R_OK) on a root-only file is EACCES",
               access(secret, R_OK) < 0 ? errno : 0, EACCES);
            errno = 0;
            ck("faccessat with no flags likewise",
               faccessat(AT_FDCWD, secret, R_OK, 0) < 0 ? errno : 0, EACCES);
            errno = 0;
            ck("faccessat(AT_EACCESS) asks about euid, and succeeds",
               faccessat(AT_FDCWD, secret, R_OK, AT_EACCESS) < 0 ? errno : 0, 0);
            fflush(NULL);
            _exit(failures_total > 250 ? 250 : (int) failures_total);
        }
        int st;
        if (waitpid(c, &st, 0) != c)
            failures_total++;
        else if (WIFSIGNALED(st)) {
            printf("FAIL child died on signal %d\n", WTERMSIG(st));
            failures_total++;
        } else
            failures_total += (unsigned) WEXITSTATUS(st);
    }

    rm_rf(base);
    return finish_suite("fs_permission_rules");
}
